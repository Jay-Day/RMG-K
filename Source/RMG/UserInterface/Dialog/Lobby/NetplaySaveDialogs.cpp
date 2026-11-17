#ifdef NETPLAY

#include "NetplaySaveDialogs.hpp"

#include <RMG-Core/Error.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;

namespace
{
constexpr int kMaxListedSaves = 200;
constexpr const char* kKindNames[CoreSaveDataKindCount] = { "EEPROM", "SRAM", "FlashRAM" };

bool isDark()
{
    return QApplication::palette().window().color().value() < 128;
}

QString okHex()
{
    return isDark() ? QStringLiteral("#66BB6A") : QStringLiteral("#2E7D32");
}

QString warnHex()
{
    return isDark() ? QStringLiteral("#FFCA28") : QStringLiteral("#B5790A");
}

QString sizeText(size_t bytes)
{
    return QObject::tr("%1 KB").arg((bytes + 1023) / 1024);
}

QString dateText(const QDateTime& when)
{
    return QLocale().toString(when, QLocale::ShortFormat);
}

// Strips what Windows can't put in a file name.
QString safeFileName(QString name)
{
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : name)
    {
        if (invalid.contains(c) || c.unicode() < 0x20)
            c = QLatin1Char('_');
    }
    return name.trimmed();
}

QString defaultSaveFolder(const QString& gameName)
{
    QString folder = QString::fromStdU32String(CoreGetNetplaySaveDirectory().u32string());
    const QString game = safeFileName(gameName);
    if (!game.isEmpty())
        folder = QDir(folder).filePath(game);
    return QDir::toNativeSeparators(folder);
}
}

QStringList UserInterface::Dialog::netplaySaveKindLabels(const CoreNetplaySaveData& data)
{
    QStringList labels;
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        if (!data.Kinds[kind].empty())
            labels << QString::fromLatin1(kKindNames[kind]);
    }
    return labels;
}

QString UserInterface::Dialog::netplaySaveSummary(const CoreNetplaySaveData& data)
{
    if (data.IsEmpty())
        return QObject::tr("blank");
    return QStringLiteral("%1 · %2").arg(netplaySaveKindLabels(data).join(QStringLiteral(" + ")),
                                         sizeText(data.TotalBytes()));
}

QString UserInterface::Dialog::netplaySaveHash(const CoreNetplaySaveData& data)
{
    std::string raw;
    CoreSerializeNetplaySaveData(data, raw);
    quint64 hash = 1469598103934665603ull;
    for (unsigned char byte : raw)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0'));
}

//
// StartingSaveDialog
//

StartingSaveDialog::StartingSaveDialog(const QString& romFile, const QString& romMd5, const QString& gameName,
                                       const std::optional<NetplaySaveChoice>& current,
                                       const std::optional<CoreNetplaySaveData>& lastMatch, QWidget* parent)
    : QDialog(parent), m_romMd5(romMd5), m_gameName(gameName)
{
    setWindowTitle(tr("Starting Save Data"));
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Everyone in the match starts from this save. During the match the game saves "
                                "to memory only, so nobody's own save files are read or changed."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_list = new QListWidget(this);
    m_list->setObjectName("StartingSaveList");
    m_list->setMinimumHeight(220);
    m_list->setUniformItemSizes(false);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_list, 1);

    auto* browseRow = new QHBoxLayout;
    auto* browseButton = new QPushButton(tr("Browse for a Save File..."), this);
    browseButton->setObjectName("StartingSaveBrowse");
    browseRow->addWidget(browseButton);
    browseRow->addStretch(1);
    layout->addLayout(browseRow);

    m_details = new QLabel(this);
    m_details->setObjectName("StartingSaveDetails");
    m_details->setTextFormat(Qt::RichText);
    m_details->setWordWrap(true);
    m_details->setMinimumHeight(48);
    layout->addWidget(m_details);

    m_continueCheck = new QCheckBox(tr("Continue from where each match left off"), this);
    m_continueCheck->setObjectName("StartingSaveContinue");
    m_continueCheck->setToolTip(tr("When a match changes the save, the next match starts from the changed save.\n"
                                   "Turn this off to start every match from the save picked here."));
    m_continueCheck->setChecked(!current || current->ContinueAfterMatch);
    layout->addWidget(m_continueCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_useButton = buttons->addButton(tr("Use This Save"), QDialogButtonBox::AcceptRole);
    m_useButton->setObjectName("StartingSaveUse");
    m_useButton->setDefault(true);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, &StartingSaveDialog::browse);
    connect(m_list, &QListWidget::currentRowChanged, this, &StartingSaveDialog::showDetails);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() {
        if (m_useButton->isEnabled())
            accept();
    });

    // For a game nobody has a save for yet: the match makes one.
    {
        Entry entry;
        entry.Title = tr("Blank save");
        entry.Detail = tr("new cartridge · nothing saved yet");
        entry.Blank = true;
        addEntry(std::move(entry), false);
    }

    // The host's own offline save for this game.
    {
        Entry entry;
        std::vector<std::filesystem::path> files;
        if (CoreReadOfflineSaveData(romFile.toStdU32String(), entry.Data, files))
        {
            QDateTime changed;
            for (const std::filesystem::path& file : files)
            {
                const QDateTime modified = QFileInfo(QString::fromStdU32String(file.u32string())).lastModified();
                if (!changed.isValid() || modified > changed)
                    changed = modified;
            }
            entry.Title = tr("Your save for this game");
            entry.Detail = tr("%1 · changed %2").arg(netplaySaveSummary(entry.Data), dateText(changed));
            entry.Native = false;
            addEntry(std::move(entry), false);
        }
    }

    if (lastMatch)
    {
        Entry entry;
        entry.Title = tr("Where the last match left off");
        entry.Detail = tr("%1 · from this room").arg(netplaySaveSummary(*lastMatch));
        entry.Data = *lastMatch;
        addEntry(std::move(entry), false);
    }

    // Netplay saves kept for this ROM, newest first.
    std::vector<Entry> kept;
    const QString folder = QString::fromStdU32String(CoreGetNetplaySaveDirectory().u32string());
    QDirIterator it(folder, { QStringLiteral("*.rmgsave") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && int(kept.size()) < kMaxListedSaves)
    {
        Entry entry;
        entry.File = it.next();
        if (!CoreReadNetplaySaveFile(entry.File.toStdU32String(), entry.Data, entry.Info) ||
            QString::fromStdString(entry.Info.RomMD5).compare(m_romMd5, Qt::CaseInsensitive) != 0)
            continue;
        entry.Title = QString::fromStdString(entry.Info.Name);
        QString detail = tr("netplay save · %1").arg(netplaySaveSummary(entry.Data));
        if (entry.Info.CreatedUnix > 0)
            detail += QStringLiteral(" · ") + dateText(QDateTime::fromSecsSinceEpoch(entry.Info.CreatedUnix));
        if (!entry.Info.Players.empty())
        {
            QStringList players;
            for (const std::string& player : entry.Info.Players)
                players << QString::fromStdString(player);
            detail += QStringLiteral(" · ") + tr("with %1").arg(players.join(QStringLiteral(", ")));
        }
        entry.Detail = detail;
        kept.push_back(std::move(entry));
    }
    std::sort(kept.begin(), kept.end(), [](const Entry& a, const Entry& b) {
        return a.Info.CreatedUnix > b.Info.CreatedUnix;
    });
    for (Entry& entry : kept)
        addEntry(std::move(entry), false);

    // Reselect what the room uses now: the same bytes, or the same file.
    // Otherwise offer the first real save, if there is one.
    int select = m_entries.size() > 1 ? 1 : 0;
    if (current)
    {
        for (int i = 0; i < int(m_entries.size()); i++)
        {
            if (m_entries[i].Data == current->Data &&
                (current->File.isEmpty() || m_entries[i].File == current->File))
            {
                select = i;
                break;
            }
        }
    }
    m_list->setCurrentRow(select);
}

void StartingSaveDialog::addEntry(Entry entry, bool select)
{
    auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(entry.Title, entry.Detail), m_list);
    item->setToolTip(QStringLiteral("%1\n%2").arg(entry.Detail,
        entry.File.isEmpty() ? entry.Title : QDir::toNativeSeparators(entry.File)));
    m_entries.push_back(std::move(entry));
    if (select)
        m_list->setCurrentItem(item);
}

void StartingSaveDialog::browse(void)
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose Save Data"),
        QString::fromStdU32String(CoreGetNetplaySaveDirectory().u32string()),
        tr("Save data (*.rmgsave *.eep *.sra *.fla);;All files (*)"));
    if (file.isEmpty())
        return;

    Entry entry;
    entry.File = file;
    if (!CoreReadNetplaySaveFile(file.toStdU32String(), entry.Data, entry.Info))
    {
        QMessageBox::warning(this, tr("Choose Save Data"),
            tr("RMG-K couldn't read %1 as save data.").arg(QFileInfo(file).fileName()));
        return;
    }

    const QString extension = QFileInfo(file).suffix().toLower();
    entry.Native = extension != QStringLiteral("rmgsave");
    entry.OtherRom = !entry.Native &&
        QString::fromStdString(entry.Info.RomMD5).compare(m_romMd5, Qt::CaseInsensitive) != 0;
    entry.Title = QString::fromStdString(entry.Info.Name);
    entry.Detail = entry.Native
        ? tr("emulator save file · %1").arg(netplaySaveSummary(entry.Data))
        : tr("netplay save · %1").arg(netplaySaveSummary(entry.Data));
    addEntry(std::move(entry), true);
}

void StartingSaveDialog::showDetails(void)
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= int(m_entries.size()))
    {
        m_details->clear();
        m_useButton->setEnabled(false);
        return;
    }

    const Entry& entry = m_entries[size_t(row)];
    if (entry.Blank)
    {
        m_details->setText(tr("%1 starts as on a new cartridge, with nothing saved. "
                              "Use Save As during the match to keep what the game saves.")
                               .arg(m_gameName.toHtmlEscaped()));
        m_useButton->setEnabled(true);
        return;
    }

    QStringList lines;
    QStringList kinds;
    for (int kind = 0; kind < CoreSaveDataKindCount; kind++)
    {
        if (entry.Data.Kinds[kind].empty())
            continue;
        kinds << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(kKindNames[kind]),
                                               sizeText(entry.Data.Kinds[kind].size()));
    }
    lines << (kinds.isEmpty() ? tr("Holds: nothing yet") : tr("Holds: %1").arg(kinds.join(QStringLiteral(", "))));

    if (entry.OtherRom)
    {
        const QString other = QString::fromStdString(entry.Info.GoodName);
        lines << QStringLiteral("<span style='color:%1; font-weight:600;'>&#9888; %2</span>")
            .arg(warnHex(), tr("Made with a different ROM (%1). Pick a save made for %2.")
                .arg(other.toHtmlEscaped(), m_gameName.toHtmlEscaped()));
    }
    else if (entry.Native)
    {
        lines << QStringLiteral("<span style='color:%1;'>&#9888; %2</span>")
            .arg(warnHex(), tr("An emulator save file doesn't say which game it's from. Make sure it's for %1.")
                .arg(m_gameName.toHtmlEscaped()));
    }
    else
    {
        lines << QStringLiteral("<span style='color:%1;'>&#10003; %2</span>")
            .arg(okHex(), tr("Made for %1.").arg(m_gameName.toHtmlEscaped()));
    }
    m_details->setText(lines.join(QStringLiteral("<br>")));
    m_useButton->setEnabled(!entry.OtherRom);
}

NetplaySaveChoice StartingSaveDialog::choice(void) const
{
    NetplaySaveChoice choice;
    const int row = m_list->currentRow();
    if (row >= 0 && row < int(m_entries.size()))
    {
        const Entry& entry = m_entries[size_t(row)];
        choice.Data = entry.Data;
        choice.Label = entry.Title;
        choice.File = entry.File;
    }
    choice.ContinueAfterMatch = m_continueCheck->isChecked();
    return choice;
}

//
// SaveNetplaySaveDialog
//

SaveNetplaySaveDialog::SaveNetplaySaveDialog(const QString& gameName, const QString& currentFile,
                                             bool keepUpdated, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Save Game Save"));
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Saves the game's save data as everyone has it now. Only your copy is written."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* nameLabel = new QLabel(tr("Name:"), this);
    m_name = new QLineEdit(this);
    m_name->setObjectName("SaveNetplayName");
    auto* folderLabel = new QLabel(tr("Folder:"), this);
    m_folder = new QLineEdit(this);
    m_folder->setObjectName("SaveNetplayFolder");
    auto* browseButton = new QPushButton(tr("Browse..."), this);

    auto* grid = new QGridLayout;
    grid->addWidget(nameLabel, 0, 0);
    grid->addWidget(m_name, 0, 1, 1, 2);
    grid->addWidget(folderLabel, 1, 0);
    grid->addWidget(m_folder, 1, 1);
    grid->addWidget(browseButton, 1, 2);
    layout->addLayout(grid);

    m_keepCheck = new QCheckBox(tr("Keep this file updated while you play"), this);
    m_keepCheck->setObjectName("SaveNetplayKeep");
    m_keepCheck->setChecked(keepUpdated);
    m_keepCheck->setToolTip(tr("Rewrites the file each time the game saves, once every player's game agrees on it."));
    layout->addWidget(m_keepCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    if (!currentFile.isEmpty())
    {
        const QFileInfo info(currentFile);
        m_name->setText(info.completeBaseName());
        m_folder->setText(QDir::toNativeSeparators(info.absolutePath()));
    }
    else
    {
        m_name->setText(safeFileName(QStringLiteral("%1 %2").arg(gameName,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm")))));
        m_folder->setText(defaultSaveFolder(gameName));
    }
    m_name->selectAll();

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a Folder"), m_folder->text());
        if (!folder.isEmpty())
            m_folder->setText(QDir::toNativeSeparators(folder));
    });
}

QString SaveNetplaySaveDialog::filePath(void) const
{
    return QDir(m_folder->text().trimmed()).filePath(safeFileName(m_name->text()) + QStringLiteral(".rmgsave"));
}

bool SaveNetplaySaveDialog::keepUpdated(void) const
{
    return m_keepCheck->isChecked();
}

void SaveNetplaySaveDialog::accept(void)
{
    if (safeFileName(m_name->text()).isEmpty() || m_folder->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, windowTitle(), tr("Give the save a name and a folder."));
        return;
    }
    if (QFileInfo::exists(filePath()) &&
        QMessageBox::question(this, windowTitle(),
            tr("%1 already exists. Replace it?").arg(QFileInfo(filePath()).fileName())) != QMessageBox::Yes)
    {
        return;
    }
    QDialog::accept();
}

#endif // NETPLAY
