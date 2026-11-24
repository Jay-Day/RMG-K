#ifdef NETPLAY

#include "GameSettingsDialog.hpp"

#include "CreateRoomDialog.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include "Utilities/Translation.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;

namespace
{
// Same options, in any order.
bool sameSet(const QStringList& a, const QStringList& b)
{
    return a.size() == b.size() && QSet<QString>(a.begin(), a.end()) == QSet<QString>(b.begin(), b.end());
}
} // namespace

//
// GameSettingsChoice
//

QString GameSettingsChoice::data() const
{
    switch (Type)
    {
    case Kind::Preset:
        return QStringLiteral("preset:") + Id;
    case Kind::Saved:
        return QStringLiteral("saved:") + Id;
    case Kind::Custom:
        return QStringLiteral("custom");
    case Kind::Default:
        break;
    }
    return QStringLiteral("default");
}

QVariantMap GameSettingsChoice::settingsValues() const
{
    static const char* const kinds[] = { "default", "preset", "saved", "custom" };
    return {
        { QStringLiteral("GameSettings"), QString::fromLatin1(kinds[int(Type)]) },
        { QStringLiteral("GameSettingsPreset"), Type == Kind::Preset ? Id : QString() },
        { QStringLiteral("GameSettingsSaved"), Type == Kind::Saved ? Id : QString() },
        { QStringLiteral("GameOptions"), Options },
    };
}

//
// GameSettingsCatalog
//

GameSettingsCatalog GameSettingsCatalog::forRom(const QString& romMd5)
{
    GameSettingsCatalog catalog;
    if (romMd5.isEmpty())
        return catalog;
    const std::string md5 = romMd5.toStdString();
    catalog.Options = CoreGetGameOptions(md5, language());
    if (catalog.Options.empty())
        return catalog;
    catalog.Groups = CoreGetGameOptionGroups(md5, language());
    catalog.Presets = CoreGetGameOptionPresets(md5, language());
    return catalog;
}

const std::string& GameSettingsCatalog::language()
{
    static const std::string language = Translation::ResolvedLanguage().toStdString();
    return language;
}

const CoreGameOptionInfo* GameSettingsCatalog::option(const QString& id) const
{
    const std::string key = id.toStdString();
    for (const CoreGameOptionInfo& info : Options)
    {
        if (info.Id == key)
            return &info;
    }
    return nullptr;
}

const CoreGameOptionPresetInfo* GameSettingsCatalog::preset(const QString& id) const
{
    const std::string key = id.toStdString();
    for (const CoreGameOptionPresetInfo& info : Presets)
    {
        if (info.Id == key)
            return &info;
    }
    return nullptr;
}

const CoreGameOptionPresetInfo* GameSettingsCatalog::presetFor(const QStringList& ids) const
{
    if (ids.isEmpty())
        return nullptr;
    QStringList sorted = ids;
    sorted.sort();
    sorted.removeDuplicates();
    for (const CoreGameOptionPresetInfo& info : Presets)
    {
        QStringList presetIds = optionIds(info);
        presetIds.sort();
        if (presetIds == sorted)
            return &info;
    }
    return nullptr;
}

QStringList GameSettingsCatalog::ordered(const QStringList& ids) const
{
    QStringList out;
    for (const CoreGameOptionInfo& info : Options)
    {
        const QString id = QString::fromStdString(info.Id);
        if (ids.contains(id))
            out << id;
    }
    return out;
}

QStringList GameSettingsCatalog::fromStored(const QStringList& ids) const
{
    // Vanilla only became Vanilla CSS when presets came in.
    QStringList mapped;
    for (const QString& id : ids)
        mapped << (id == QLatin1String("vanilla-only") ? QStringLiteral("vanilla-css") : id);
    return ordered(mapped);
}

QString GameSettingsCatalog::problemWith(const QStringList& ids) const
{
    if (ids.size() > CoreGameOptionsMaxOptions)
        return tr("It has more than %1 options on.").arg(CoreGameOptionsMaxOptions);
    for (const QString& id : ids)
    {
        const CoreGameOptionInfo* info = option(id);
        if (!info)
            continue;
        for (const std::string& required : info->Requires)
        {
            const QString requiredId = QString::fromStdString(required);
            if (!ids.contains(requiredId))
                return tr("%1 needs %2, which is off.").arg(labels({ id }).value(0), labels({ requiredId }).value(0));
        }
        for (const std::string& conflict : info->Conflicts)
        {
            const QString conflictId = QString::fromStdString(conflict);
            if (ids.contains(conflictId))
                return tr("%1 and %2 can't be on together.").arg(labels({ id }).value(0), labels({ conflictId }).value(0));
        }
    }
    return QString();
}

QStringList GameSettingsCatalog::optionIds(const CoreGameOptionPresetInfo& preset)
{
    QStringList ids;
    for (const std::string& id : preset.Options)
        ids << QString::fromStdString(id);
    return ids;
}

QStringList GameSettingsCatalog::labels(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids)
    {
        const CoreGameOptionInfo* info = option(id);
        out << (info ? QString::fromStdString(info->Label) : id);
    }
    return out;
}

QList<SavedGameSettings> GameSettingsCatalog::savedSets(const QString& romMd5) const
{
    QList<SavedGameSettings> sets;
    if (romMd5.isEmpty() || isEmpty())
        return sets;
    QSettings s("RMG-K", "n02");
    s.beginGroup(CreateRoomDialog::romSettingsGroup(romMd5));
    const QJsonArray list = QJsonDocument::fromJson(s.value("SavedGameSettings").toString().toUtf8()).array();
    s.endGroup();
    for (const QJsonValue& value : list)
    {
        const QJsonObject o = value.toObject();
        SavedGameSettings set;
        set.Id = o.value("id").toString();
        // Hand-edited entries the lobby wouldn't take are left out.
        static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{8}$"));
        QString error;
        set.Name = cleanSavedName(o.value("name").toString(), &error);
        if (!idPattern.match(set.Id).hasMatch() || set.Name.isEmpty() || findSaved(sets, set.Id))
            continue;
        QStringList stored;
        for (const QJsonValue& id : o.value("options").toArray())
            stored << id.toString();
        set.Options = fromStored(stored);
        set.Problem = set.Options.isEmpty() ? tr("None of its options are in your game options packs any more.")
                                            : problemWith(set.Options);
        sets << set;
    }
    std::sort(sets.begin(), sets.end(), [](const SavedGameSettings& a, const SavedGameSettings& b) {
        const int byName = QString::localeAwareCompare(a.Name.toCaseFolded(), b.Name.toCaseFolded());
        return byName != 0 ? byName < 0 : a.Id < b.Id;
    });
    return sets;
}

void GameSettingsCatalog::storeSavedSets(const QString& romMd5, const QList<SavedGameSettings>& sets)
{
    QJsonArray list;
    for (const SavedGameSettings& set : sets)
    {
        QJsonObject o;
        o["id"] = set.Id;
        o["name"] = set.Name;
        o["options"] = QJsonArray::fromStringList(set.Options);
        list.append(o);
    }
    QSettings s("RMG-K", "n02");
    s.beginGroup(CreateRoomDialog::romSettingsGroup(romMd5));
    if (list.isEmpty())
        s.remove("SavedGameSettings");
    else
        s.setValue("SavedGameSettings", QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact)));
    s.endGroup();
}

const SavedGameSettings* GameSettingsCatalog::findSaved(const QList<SavedGameSettings>& sets, const QString& id)
{
    for (const SavedGameSettings& set : sets)
    {
        if (set.Id == id)
            return &set;
    }
    return nullptr;
}

QString GameSettingsCatalog::cleanSavedName(const QString& text, QString* error)
{
    // The lobby's rules for a preset's label (it refuses the whole change
    // otherwise): 1 to 48 characters, trimmed, no control, line break or
    // direction characters.
    const QString name = text.trimmed();
    if (name.isEmpty())
    {
        *error = tr("Give these settings a name.");
        return QString();
    }
    const QList<uint> points = name.toUcs4();
    if (points.size() > MaxSavedNameLength)
    {
        *error = tr("A name can be up to %1 characters long.").arg(MaxSavedNameLength);
        return QString();
    }
    for (const uint c : points)
    {
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029 || (c >= 0x202a && c <= 0x202e) ||
            (c >= 0x2066 && c <= 0x2069) || c == 0xfffd)
        {
            *error = tr("A name can't have line breaks or other control characters.");
            return QString();
        }
    }
    error->clear();
    return name;
}

QString GameSettingsCatalog::savedNameFor(const QString& text, QString* error) const
{
    const QString name = cleanSavedName(text, error);
    if (name.isEmpty())
        return name;
    // Not one of the list's own entries, which it would pass for.
    QStringList taken = { tr("Default"), QStringLiteral("Default"), tr("Custom"), QStringLiteral("Custom") };
    for (const CoreGameOptionPresetInfo& preset : Presets)
        taken << QString::fromStdString(preset.Label) << QString::fromStdString(preset.EnglishLabel);
    for (const QString& other : std::as_const(taken))
    {
        if (name.compare(other, Qt::CaseInsensitive) == 0)
        {
            *error = tr("\"%1\" is already one of the Game settings list's entries.").arg(other);
            return QString();
        }
    }
    error->clear();
    return name;
}

QString GameSettingsCatalog::newSavedId(const QList<SavedGameSettings>& sets)
{
    QString id;
    do
        id = QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    while (findSaved(sets, id));
    return id;
}

GameSettingsChoice GameSettingsCatalog::choiceFor(const QStringList& on, const QList<SavedGameSettings>& saved,
                                                  const QString& savedHint) const
{
    GameSettingsChoice choice;
    choice.Options = ordered(on);
    if (choice.Options.isEmpty())
        return choice;
    if (const CoreGameOptionPresetInfo* info = presetFor(choice.Options))
    {
        choice.Type = GameSettingsChoice::Kind::Preset;
        choice.Id = QString::fromStdString(info->Id);
        return choice;
    }
    const auto matches = [&](const SavedGameSettings& set) {
        return set.Problem.isEmpty() && sameSet(set.Options, choice.Options);
    };
    const SavedGameSettings* hinted = savedHint.isEmpty() ? nullptr : findSaved(saved, savedHint);
    const SavedGameSettings* match = hinted && matches(*hinted) ? hinted : nullptr;
    for (int i = 0; !match && i < saved.size(); ++i)
        match = matches(saved.at(i)) ? &saved.at(i) : nullptr;
    if (match)
    {
        choice.Type = GameSettingsChoice::Kind::Saved;
        choice.Id = match->Id;
        return choice;
    }
    choice.Type = GameSettingsChoice::Kind::Custom;
    return choice;
}

GameSettingsChoice GameSettingsCatalog::rememberedChoice(const QSettings& s, const QList<SavedGameSettings>& saved) const
{
    const QString kind = s.value("GameSettings").toString();
    if (kind == QLatin1String("default"))
        return {};
    if (kind == QLatin1String("saved"))
    {
        // Followed by its id through renames and edits; gone means Default.
        const SavedGameSettings* set = findSaved(saved, s.value("GameSettingsSaved").toString());
        return set && set->Problem.isEmpty() ? choiceFor(set->Options, saved, set->Id) : GameSettingsChoice();
    }
    if (kind == QLatin1String("preset"))
    {
        // By id, so it gets the options the pack gives it now.
        if (const CoreGameOptionPresetInfo* info = preset(s.value("GameSettingsPreset").toString()))
            return choiceFor(optionIds(*info), saved);
    }
    // Custom, or a room from before presets that only kept the options.
    const QStringList stored = fromStored(s.value("GameOptions").toStringList());
    return problemWith(stored).isEmpty() ? choiceFor(stored, saved) : GameSettingsChoice();
}

GameSettingsChoice GameSettingsCatalog::choiceFromData(const QString& data, const QList<SavedGameSettings>& saved,
                                                       const QStringList& custom) const
{
    GameSettingsChoice choice;
    if (data.startsWith(QLatin1String("preset:")))
    {
        if (const CoreGameOptionPresetInfo* info = preset(data.mid(7)))
        {
            choice.Type = GameSettingsChoice::Kind::Preset;
            choice.Id = QString::fromStdString(info->Id);
            choice.Options = optionIds(*info);
        }
    }
    else if (data.startsWith(QLatin1String("saved:")))
    {
        const SavedGameSettings* set = findSaved(saved, data.mid(6));
        if (set && set->Problem.isEmpty())
        {
            choice.Type = GameSettingsChoice::Kind::Saved;
            choice.Id = set->Id;
            choice.Options = set->Options;
        }
    }
    else if (data == QLatin1String("custom") && !custom.isEmpty())
    {
        choice.Type = GameSettingsChoice::Kind::Custom;
        choice.Options = ordered(custom);
    }
    return choice;
}

QList<GameSettingsListItem> GameSettingsCatalog::listItems(const QList<SavedGameSettings>& saved,
                                                           const GameSettingsChoice& current, int limit,
                                                           int* currentIndex) const
{
    const QString tooMany = tr("This lobby server takes at most %1 game options.").arg(limit);
    QList<GameSettingsListItem> items;
    items.append({ tr("Default"), QStringLiteral("default"), tr("The game plays unchanged.") });
    for (const CoreGameOptionPresetInfo& info : Presets)
    {
        const bool fits = int(info.Options.size()) <= limit;
        items.append({ QString::fromStdString(info.Label), QStringLiteral("preset:") + QString::fromStdString(info.Id),
                       fits ? QString::fromStdString(info.Description) : tooMany, fits });
    }
    // The host's own, set apart from the packs' presets.
    if (!saved.isEmpty())
    {
        items.append({ QString(), QString(), QString(), false, true });
        for (const SavedGameSettings& set : saved)
        {
            const bool fits = set.Options.size() <= limit;
            const QString tip = !set.Problem.isEmpty() ? tr("Can't be used: %1").arg(set.Problem)
                              : !fits ? tooMany
                                      : tr("Your saved settings: %1").arg(labels(set.Options).join(QStringLiteral(", ")));
            items.append({ set.Name, QStringLiteral("saved:") + set.Id, tip, set.Problem.isEmpty() && fits });
        }
        items.append({ QString(), QString(), QString(), false, true });
    }
    const bool custom = current.Type == GameSettingsChoice::Kind::Custom;
    items.append({ custom ? tr("Custom (%1)", "game settings that aren't a preset, with how many options are on")
                                .arg(current.Options.size())
                          : tr("Custom..."),
                   QStringLiteral("custom"),
                   custom ? labels(current.Options).join(QStringLiteral(", ")) : tr("Turn the game's options on and off one by one.") });
    if (currentIndex)
    {
        const QString data = current.data();
        *currentIndex = 0;
        for (int i = 0; i < items.size(); ++i)
        {
            if (!items.at(i).Separator && items.at(i).Data == data)
                *currentIndex = i;
        }
    }
    return items;
}

bool GameSettingsCatalog::fillList(QComboBox* combo, const QList<GameSettingsListItem>& items)
{
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    bool same = model && items.size() == combo->count();
    for (int i = 0; same && i < items.size(); ++i)
    {
        const GameSettingsListItem& item = items.at(i);
        const bool separator = combo->itemData(i, Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
        same = separator == item.Separator &&
            (separator || (combo->itemText(i) == item.Text && combo->itemData(i).toString() == item.Data &&
                           combo->itemData(i, Qt::ToolTipRole).toString() == item.Tip &&
                           model->item(i)->isEnabled() == item.Enabled));
    }
    if (same)
        return false;
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const GameSettingsListItem& item : items)
    {
        if (item.Separator)
        {
            combo->insertSeparator(combo->count());
            continue;
        }
        combo->addItem(item.Text, item.Data);
        const int index = combo->count() - 1;
        combo->setItemData(index, item.Tip, Qt::ToolTipRole);
        if (!item.Enabled && model)
            model->item(index)->setEnabled(false);
    }
    return true;
}

//
// GameSettingsDialog
//

namespace
{
QLabel* headingLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text.toUpper(), parent);
    QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    label->setFont(font);
    label->setProperty("role", "muted");
    return label;
}

QFrame* rule(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setObjectName("FieldRule");
    return line;
}

QLabel* smallText(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(LobbyTheme::uiFont(11));
    label->setProperty("role", role);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

// The switch's text starts this far in: the lines under it line up with it.
constexpr int kTextIndent = 41;

// A line under an option saying what goes with it, behind a small icon.
QWidget* relationLine(const QString& icon, const QString& text, QWidget* parent)
{
    auto* line = new QWidget(parent);
    auto* lay = new QHBoxLayout(line);
    lay->setContentsMargins(kTextIndent, 0, 0, 0);
    lay->setSpacing(5);
    lay->addWidget(new IconLabel(icon, LobbyTheme::Role::Muted, 12, line), 0, Qt::AlignTop);
    lay->addWidget(smallText(text, "muted", line), 1);
    return line;
}

// A switch that shows a state without taking clicks, for the read-only view:
// disabled ones are drawn faded, which would hide what's on.
void makeReadOnly(ToggleSwitch* toggle)
{
    toggle->setAttribute(Qt::WA_TransparentForMouseEvents);
    toggle->setFocusPolicy(Qt::NoFocus);
    toggle->setCursor(Qt::ArrowCursor);
}

QString joinLabels(const QStringList& labels)
{
    return labels.join(QStringLiteral(", "));
}
} // namespace

GameSettingsDialog::GameSettingsDialog(const QString& gameName, GameSettingsCatalog catalog, const QStringList& on,
                                       bool editable, int limit, QWidget* parent)
    : QDialog(parent), m_catalog(std::move(catalog)), m_editable(editable),
      m_limit(std::clamp(limit, 1, CoreGameOptionsMaxOptions)), m_gameName(gameName)
{
    setWindowTitle(tr("Game Settings"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setMinimumWidth(560);
    resize(600, 620);

    m_on = m_catalog.ordered(on);
    for (const CoreGameOptionInfo& option : m_catalog.Options)
    {
        for (const std::string& required : option.Requires)
            m_neededBy[QString::fromStdString(required)] << QString::fromStdString(option.Id);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The band: the game's tile, the title and the game's name.
    auto* header = new LobbyBand(this);
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(18, 12, 18, 12);
    headerLay->setSpacing(12);
    m_tile = new CartridgeTile(CartridgeTile::Size::Small, header);
    m_tile->setGame(gameName);
    headerLay->addWidget(m_tile);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel(tr("Game Settings").toUpper(), header);
    title->setFont(LobbyTheme::pixelFont(15, 0.5));
    titles->addWidget(title);
    auto* gameLabel = new QLabel(gameName.isEmpty() ? QStringLiteral("—") : gameName, header);
    gameLabel->setFont(LobbyTheme::uiFont(11));
    gameLabel->setProperty("role", "muted");
    titles->addWidget(gameLabel);
    headerLay->addLayout(titles, 1);
    root->addWidget(header);
    root->addWidget(rule(this));

    auto* body = new QVBoxLayout;
    m_body = body;
    body->setContentsMargins(18, 14, 18, 10);
    body->setSpacing(10);
    auto* intro = smallText(editable
        ? tr("Every player's game is changed this way for the next match, and so is its replay. "
             "Pick a preset from the Game Settings list, or turn options on here one by one.")
        : tr("What the host turned on for the next match. Every player's game is changed this way, "
             "and so is its replay."), "muted", this);
    body->addWidget(intro);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("GameSettingsScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content = new QWidget(scroll);
    auto* sections = new QVBoxLayout(content);
    m_sections = sections;
    sections->setContentsMargins(0, 4, 6, 4);
    sections->setSpacing(16);

    // The pack's groups in its order, then whatever has none.
    const auto addSection = [&](const QString& heading) {
        auto* box = new QWidget(content);
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(10);
        if (!heading.isEmpty())
            lay->addWidget(headingLabel(heading, box));
        sections->addWidget(box);
        return lay;
    };
    const auto inGroup = [](const CoreGameOptionInfo& option, const std::string& group) {
        return option.Group == group;
    };
    std::vector<std::string> listed;
    for (const CoreGameOptionGroupInfo& group : m_catalog.Groups)
    {
        const bool used = std::any_of(m_catalog.Options.begin(), m_catalog.Options.end(),
            [&](const CoreGameOptionInfo& option) { return inGroup(option, group.Id); });
        if (!used)
            continue;
        QVBoxLayout* section = addSection(QString::fromStdString(group.Label));
        for (const CoreGameOptionInfo& option : m_catalog.Options)
        {
            if (inGroup(option, group.Id))
                addOptionRow(section, option);
        }
        listed.push_back(group.Id);
    }
    QVBoxLayout* ungrouped = nullptr;
    for (const CoreGameOptionInfo& option : m_catalog.Options)
    {
        if (std::find(listed.begin(), listed.end(), option.Group) != listed.end())
            continue;
        if (!ungrouped)
            ungrouped = addSection(listed.empty() ? QString() : tr("Other", "game options without a group"));
        addOptionRow(ungrouped, option);
    }
    // On in the room, but this client's packs don't have them.
    m_otherSection = addSection(tr("Not in your game options packs"));
    m_otherBox = m_otherSection->parentWidget();
    m_otherBox->setVisible(false);
    sections->addStretch(1);
    scroll->setWidget(content);
    content->setAutoFillBackground(false); // setWidget turns it on
    body->addWidget(scroll, 1);

    // Where the set stands: how many are on, and whether it's a preset.
    auto* status = new QHBoxLayout;
    status->setSpacing(10);
    m_count = new QLabel(this);
    m_count->setObjectName("GameSettingsCount");
    m_count->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    status->addWidget(m_count);
    m_matches = smallText(QString(), "muted", this);
    m_matches->setObjectName("GameSettingsMatches");
    m_matches->setWordWrap(false);
    status->addWidget(m_matches, 1);
    body->addLayout(status);
    m_note = smallText(QString(), "ink2", this);
    m_note->setObjectName("GameSettingsNote");
    m_note->setVisible(false);
    body->addWidget(m_note);
    m_error = smallText(QString(), "bad", this);
    m_error->setObjectName("GameSettingsError");
    m_error->setVisible(false);
    body->addWidget(m_error);
    root->addLayout(body, 1);
    root->addWidget(rule(this));

    auto* footer = new QHBoxLayout;
    m_footer = footer;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    footer->addStretch(1);
    if (editable)
    {
        auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
        cancel->setObjectName("GameSettingsCancel");
        cancel->setButtonSize(LobbyButton::Size::Large);
        m_applyButton = new LobbyButton(tr("Apply"), LobbyButton::Kind::Primary, this);
        m_applyButton->setObjectName("GameSettingsApply");
        m_applyButton->setIconName(QStringLiteral("check-line"));
        m_applyButton->setButtonSize(LobbyButton::Size::Large);
        m_applyButton->setDefault(true);
        footer->addWidget(cancel);
        footer->addWidget(m_applyButton);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_applyButton, &QPushButton::clicked, this, &GameSettingsDialog::apply);
        m_mainButton = m_applyButton;
    }
    else
    {
        auto* close = new LobbyButton(tr("Close"), LobbyButton::Kind::Primary, this);
        close->setObjectName("GameSettingsClose");
        close->setButtonSize(LobbyButton::Size::Large);
        close->setDefault(true);
        footer->addWidget(close);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        m_mainButton = close;
    }
    root->addLayout(footer);

    refresh();
}

void GameSettingsDialog::addOptionRow(QVBoxLayout* section, const CoreGameOptionInfo& option)
{
    const QString id = QString::fromStdString(option.Id);
    QWidget* parent = section->parentWidget();
    auto* row = new QWidget(parent);
    auto* lay = new QVBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(3);

    auto* optionSwitch = new ToggleSwitch(QString::fromStdString(option.Label), row);
    optionSwitch->setObjectName("GameOptionSwitch");
    optionSwitch->setProperty("gameOptionId", id);
    optionSwitch->setAccessibleName(QString::fromStdString(option.Label));
    if (m_editable)
    {
        connect(optionSwitch, &QCheckBox::toggled, this, [this, id](bool checked) { toggle(id, checked); });
    }
    else
    {
        makeReadOnly(optionSwitch);
        // Accessibility can still flip it; it shows the room's state, so it goes back.
        connect(optionSwitch, &QCheckBox::toggled, this, &GameSettingsDialog::refresh);
    }
    lay->addWidget(optionSwitch);

    if (!option.Description.empty())
    {
        auto* description = smallText(QString::fromStdString(option.Description), "muted", row);
        description->setContentsMargins(kTextIndent, 0, 0, 0);
        lay->addWidget(description);
    }

    // What goes on or off with it, said where it can't be missed.
    // In the order the list shows them.
    QStringList needs;
    for (const std::string& required : option.Requires)
        needs << QString::fromStdString(required);
    needs = m_catalog.ordered(needs);
    QStringList conflicts;
    for (const std::string& conflict : option.Conflicts)
        conflicts << QString::fromStdString(conflict);
    conflicts = m_catalog.ordered(conflicts);
    const QStringList neededBy = m_catalog.ordered(m_neededBy.value(id));
    if (!needs.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("add-line"), m_editable
            ? tr("Turning this on also turns on %1.").arg(joinLabels(labelsOf(needs)))
            : tr("Needs %1.").arg(joinLabels(labelsOf(needs))), row));
    if (!neededBy.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("subtract-line"), m_editable
            ? tr("Turning this off also turns off %1.").arg(joinLabels(labelsOf(neededBy)))
            : tr("Needed by %1.").arg(joinLabels(labelsOf(neededBy))), row));
    if (!conflicts.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("forbid-line"), m_editable
            ? tr("Turning this on turns off %1.").arg(joinLabels(labelsOf(conflicts)))
            : tr("Can't be on with %1.").arg(joinLabels(labelsOf(conflicts))), row));

    section->addWidget(row);
    m_rows.push_back({ id, optionSwitch });
}

QString GameSettingsDialog::labelOf(const QString& id) const
{
    const CoreGameOptionInfo* option = m_catalog.option(id);
    return option ? QString::fromStdString(option->Label) : id;
}

QStringList GameSettingsDialog::labelsOf(const QStringList& ids) const
{
    QStringList labels;
    for (const QString& id : ids)
        labels << labelOf(id);
    return labels;
}

void GameSettingsDialog::toggle(const QString& id, bool on)
{
    QStringList next = m_on;
    QStringList turnedOn, offConflicting, offDependent;

    // Takes id out, and everything that needs it, and what needs those.
    const auto removeWithDependents = [&](const QString& first, QStringList* removed) {
        QStringList queue = { first };
        for (int i = 0; i < queue.size(); ++i)
        {
            if (!next.removeOne(queue.at(i)))
                continue;
            if (i == 0 && removed)
                removed->append(queue.at(i));
            else if (i > 0)
                offDependent.append(queue.at(i));
            for (const QString& dependent : m_neededBy.value(queue.at(i)))
            {
                if (!queue.contains(dependent))
                    queue << dependent;
            }
        }
    };

    if (on)
    {
        // It and what it needs, and what that needs.
        QStringList needed = { id };
        for (int i = 0; i < needed.size(); ++i)
        {
            if (const CoreGameOptionInfo* option = m_catalog.option(needed.at(i)))
            {
                for (const std::string& required : option->Requires)
                {
                    const QString requiredId = QString::fromStdString(required);
                    if (!needed.contains(requiredId))
                        needed << requiredId;
                }
            }
        }
        for (const QString& neededId : std::as_const(needed))
        {
            if (!next.contains(neededId))
            {
                next << neededId;
                if (neededId != id)
                    turnedOn << neededId;
            }
        }
        // The pack never makes an option need two that conflict, so nothing
        // taken out here is one of those.
        for (const QString& neededId : std::as_const(needed))
        {
            if (const CoreGameOptionInfo* option = m_catalog.option(neededId))
            {
                for (const std::string& conflict : option->Conflicts)
                    removeWithDependents(QString::fromStdString(conflict), &offConflicting);
            }
        }
    }
    else
    {
        removeWithDependents(id, nullptr);
    }

    m_error->setVisible(false);
    if (next.size() > m_limit)
    {
        m_note->setText(tr("At most %1 game options can be on at once.").arg(m_limit));
        m_note->setVisible(true);
        refresh();
        return;
    }
    m_on = m_catalog.ordered(next);

    QStringList notes;
    if (!turnedOn.isEmpty())
        notes << tr("Turned on %1 too, for %2.").arg(joinLabels(labelsOf(turnedOn)), labelOf(id));
    if (!offConflicting.isEmpty())
        notes << tr("Turned off %1, which can't be on with %2.").arg(joinLabels(labelsOf(offConflicting)), labelOf(id));
    if (!offDependent.isEmpty())
        notes << tr("Turned off %1 too, which needs %2.").arg(joinLabels(labelsOf(offDependent)),
            on ? joinLabels(labelsOf(offConflicting)) : labelOf(id));
    m_note->setText(notes.join(QLatin1Char('\n')));
    m_note->setVisible(!notes.isEmpty());
    refresh();
}

void GameSettingsDialog::refresh()
{
    for (const Row& row : m_rows)
    {
        const bool on = m_on.contains(row.Id);
        {
            const QSignalBlocker blocker(row.Switch);
            row.Switch->setChecked(on);
        }
        if (!m_editable)
            continue;
        // An option that would take the set past the limit waits for room.
        bool fits = on;
        if (!fits)
        {
            int added = 0;
            QStringList needed = { row.Id };
            for (int i = 0; i < needed.size(); ++i)
            {
                if (!m_on.contains(needed.at(i)))
                    added++;
                if (const CoreGameOptionInfo* option = m_catalog.option(needed.at(i)))
                {
                    for (const std::string& required : option->Requires)
                    {
                        const QString requiredId = QString::fromStdString(required);
                        if (!needed.contains(requiredId))
                            needed << requiredId;
                    }
                }
            }
            int removed = 0;
            for (const QString& neededId : std::as_const(needed))
            {
                if (const CoreGameOptionInfo* option = m_catalog.option(neededId))
                {
                    for (const std::string& conflict : option->Conflicts)
                        removed += m_on.contains(QString::fromStdString(conflict)) ? 1 : 0;
                }
            }
            fits = m_on.size() + added - removed <= m_limit;
        }
        row.Switch->setEnabled(fits);
        row.Switch->setToolTip(fits ? QString() : tr("At most %1 game options can be on at once.").arg(m_limit));
    }

    // Options on that the packs don't know count too (read-only view only).
    const int otherCount = int(m_otherLabels.size());
    const int count = int(m_on.size()) + otherCount;
    m_count->setText(m_editable ? tr("%1 of %2 on").arg(count).arg(m_limit) : tr("%n on", nullptr, count));
    m_count->setToolTip(m_limitNote);

    const CoreGameOptionPresetInfo* preset = otherCount == 0 ? m_catalog.presetFor(m_on) : nullptr;
    const SavedGameSettings* saved = otherCount == 0 && !preset ? savedMatching(m_on) : nullptr;
    QString matches;
    if (count == 0)
        matches = tr("Default: the game plays unchanged.");
    else if (preset)
        matches = tr("Same as the %1 preset.").arg(QString::fromStdString(preset->Label));
    else if (saved)
        matches = tr("Same as your saved \"%1\".").arg(saved->Name);
    // Read-only: the host's name for it, their own saved set's or a preset
    // this client's packs don't have.
    else if (!m_editable && !m_roomPresetLabel.isEmpty())
        matches = m_roomSavedByHost ? tr("The host's own settings, saved as \"%1\".").arg(m_roomPresetLabel)
                                    : tr("Same as the %1 preset.").arg(m_roomPresetLabel);
    else
        matches = m_saveAsButton ? tr("Custom: not one of the presets or your saved settings.")
                                 : tr("Custom: not one of the presets.");
    if (!m_limitNote.isEmpty() && m_editable)
        matches += QStringLiteral("  ·  ") + m_limitNote;
    m_matches->setText(matches);

    // Only a set that isn't in the list already can be saved.
    if (m_saveAsButton)
    {
        const QString why = saveBlocker();
        m_saveAsButton->setEnabled(why.isEmpty());
        m_saveAsButton->setToolTip(!why.isEmpty() ? why
                                   : m_editable ? tr("Save these settings under a name of your own, to pick them "
                                                     "from the Game settings list next time.")
                                                : tr("Save these settings under a name of your own, to pick them "
                                                     "from the Game settings list when you host."));
        if (!m_saveBar->isHidden())
            checkSaveName();
    }
}

QString GameSettingsDialog::saveBlocker() const
{
    if (!m_editable)
    {
        // The room's set as it is: a part of it would be other rules.
        if (!m_otherLabels.isEmpty())
            return tr("Your game options packs don't have %1, so these settings can't be saved here.")
                .arg(joinLabels(m_otherLabels));
        if (m_on.isEmpty())
            return tr("No options are on, so there's nothing to save.");
        // Packs that differ from the host's may not allow the set.
        const QString problem = m_catalog.problemWith(m_on);
        if (!problem.isEmpty())
            return tr("Your game options packs don't allow these settings: %1").arg(problem);
    }
    if (m_on.isEmpty())
        return tr("Turn some options on to save them.");
    if (const CoreGameOptionPresetInfo* preset = m_catalog.presetFor(m_on))
        return tr("These are the %1 preset's settings.").arg(QString::fromStdString(preset->Label));
    if (const SavedGameSettings* saved = savedMatching(m_on))
        return m_editable ? tr("These are saved as \"%1\".").arg(saved->Name)
                          : tr("You already have these saved as \"%1\".").arg(saved->Name);
    return QString();
}

void GameSettingsDialog::setCoverPath(const QString& coverPath)
{
    if (m_tile)
        m_tile->setGame(m_gameName, coverPath);
}

void GameSettingsDialog::setLimitNote(const QString& note)
{
    m_limitNote = note;
    refresh();
}

void GameSettingsDialog::showRoomOptions(const QList<LobbyClient::LobbyGameOption>& on, const QString& presetLabel,
                                         bool savedByHost)
{
    QStringList ids, known, otherLabels;
    QList<LobbyClient::LobbyGameOption> unknown;
    for (const LobbyClient::LobbyGameOption& option : on)
    {
        ids << option.id;
        if (m_catalog.option(option.id))
        {
            known << option.id;
        }
        else
        {
            unknown << option;
            otherLabels << option.label;
        }
    }
    // The room's list is sent again for every room update; only a change
    // matters here.
    if (m_roomShown && ids == m_roomIds && otherLabels == m_otherLabels && presetLabel == m_roomPresetLabel &&
        savedByHost == m_roomSavedByHost)
        return;
    if (m_roomShown && m_saveBar)
    {
        // A name typed for the old settings isn't one for the new ones.
        const bool naming = !m_saveBar->isHidden();
        closeSaveBar();
        m_note->setText(tr("The host changed the game settings while you were naming them, so nothing was saved."));
        m_note->setVisible(naming);
        // Saved sets made elsewhere since count too.
        m_saved = m_catalog.savedSets(m_romMd5);
    }
    m_roomShown = true;
    m_roomIds = ids;
    m_otherLabels = otherLabels;
    m_roomPresetLabel = presetLabel;
    m_roomSavedByHost = savedByHost;
    m_on = m_catalog.ordered(known);

    while (m_otherSection->count() > 1)
    {
        QLayoutItem* item = m_otherSection->takeAt(1);
        delete item->widget();
        delete item;
    }
    for (const LobbyClient::LobbyGameOption& option : std::as_const(unknown))
    {
        auto* otherSwitch = new ToggleSwitch(option.label, m_otherBox);
        otherSwitch->setObjectName("GameOptionSwitch");
        otherSwitch->setProperty("gameOptionId", option.id);
        otherSwitch->setChecked(true);
        makeReadOnly(otherSwitch);
        connect(otherSwitch, &QCheckBox::toggled, otherSwitch, [otherSwitch]() {
            const QSignalBlocker blocker(otherSwitch);
            otherSwitch->setChecked(true);
        });
        m_otherSection->addWidget(otherSwitch);
    }
    m_otherBox->setVisible(!unknown.isEmpty());
    // The status line names the host's saved set or preset (refresh).
    refresh();
}

void GameSettingsDialog::setApplyCheck(std::function<bool(const QStringList&, QString&)> check)
{
    m_check = std::move(check);
}

QStringList GameSettingsDialog::chosen() const
{
    return m_on;
}

void GameSettingsDialog::apply()
{
    QString error;
    if (m_check && !m_check(chosen(), error))
    {
        m_error->setText(error);
        m_error->setVisible(true);
        return;
    }
    accept();
}

//
// Saved sets
//

namespace
{
void setLabelRole(QLabel* label, const char* role)
{
    if (label->property("role").toString() == QLatin1String(role))
        return;
    label->setProperty("role", role);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

LobbyButton* rowButton(const QString& text, LobbyButton::Kind kind, const char* name, QWidget* parent)
{
    auto* button = new LobbyButton(text, kind, parent);
    button->setObjectName(name);
    button->setButtonSize(LobbyButton::Size::Compact);
    return button;
}
} // namespace

void GameSettingsDialog::enableSavedSets(const QString& romMd5)
{
    if (romMd5.isEmpty() || m_saveAsButton)
        return;
    m_romMd5 = romMd5;
    m_saved = m_catalog.savedSets(romMd5);

    // The saved sets come first, above the pack's groups. Only the host gets
    // them: loading one would change the room.
    if (m_editable)
    {
        m_savedBox = new QWidget(m_sections->parentWidget());
        m_savedBox->setObjectName("SavedSettingsSection");
        auto* savedLay = new QVBoxLayout(m_savedBox);
        savedLay->setContentsMargins(0, 0, 0, 0);
        savedLay->setSpacing(6);
        savedLay->addWidget(headingLabel(tr("Your saved settings"), m_savedBox));
        m_savedRows = new QVBoxLayout;
        m_savedRows->setSpacing(4);
        savedLay->addLayout(m_savedRows);
        m_sections->insertWidget(0, m_savedBox);
    }

    // Save as: a name field under the switches' status, not a dialog of its own.
    m_saveBar = new QWidget(this);
    m_saveBar->setObjectName("GameSettingsSaveBar");
    auto* barLay = new QVBoxLayout(m_saveBar);
    barLay->setContentsMargins(0, 2, 0, 0);
    barLay->setSpacing(5);
    barLay->addWidget(headingLabel(tr("Save as"), m_saveBar));
    auto* fieldRow = new QHBoxLayout;
    fieldRow->setSpacing(8);
    m_saveName = new QLineEdit(m_saveBar);
    m_saveName->setObjectName("GameSettingsSaveName");
    m_saveName->setMaxLength(GameSettingsCatalog::MaxSavedNameLength);
    m_saveName->setMinimumHeight(30);
    m_saveName->setPlaceholderText(tr("A name, like Friday rules"));
    m_saveName->setAccessibleName(tr("Name for these settings"));
    fieldRow->addWidget(m_saveName, 1);
    m_saveConfirm = new LobbyButton(tr("Save"), LobbyButton::Kind::Primary, m_saveBar);
    m_saveConfirm->setObjectName("GameSettingsSaveConfirm");
    m_saveConfirm->setButtonSize(LobbyButton::Size::Small);
    fieldRow->addWidget(m_saveConfirm);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, m_saveBar);
    cancel->setObjectName("GameSettingsSaveCancel");
    cancel->setButtonSize(LobbyButton::Size::Small);
    fieldRow->addWidget(cancel);
    barLay->addLayout(fieldRow);
    m_saveMessage = smallText(QString(), "ink2", m_saveBar);
    m_saveMessage->setObjectName("GameSettingsSaveMessage");
    m_saveMessage->setVisible(false);
    barLay->addWidget(m_saveMessage);
    m_saveBar->setVisible(false);
    m_body->addWidget(m_saveBar);
    connect(m_saveName, &QLineEdit::textChanged, this, &GameSettingsDialog::checkSaveName);
    connect(m_saveConfirm, &QPushButton::clicked, this, &GameSettingsDialog::saveAs);
    connect(cancel, &QPushButton::clicked, this, &GameSettingsDialog::closeSaveBar);

    m_saveAsButton = new LobbyButton(tr("Save as..."), LobbyButton::Kind::Secondary, this);
    m_saveAsButton->setObjectName("GameSettingsSaveAs");
    m_saveAsButton->setIconName(QStringLiteral("add-line"));
    m_saveAsButton->setButtonSize(LobbyButton::Size::Large);
    m_footer->insertWidget(0, m_saveAsButton);
    connect(m_saveAsButton, &QPushButton::clicked, this, &GameSettingsDialog::openSaveBar);

    rebuildSavedSection();
    refresh();
}

const SavedGameSettings* GameSettingsDialog::savedMatching(const QStringList& ids) const
{
    if (ids.isEmpty())
        return nullptr;
    for (const SavedGameSettings& set : m_saved)
    {
        if (set.Problem.isEmpty() && sameSet(set.Options, ids))
            return &set;
    }
    return nullptr;
}

void GameSettingsDialog::rebuildSavedSection()
{
    if (!m_savedRows)
        return;
    while (QLayoutItem* item = m_savedRows->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_savedBox->setVisible(!m_saved.isEmpty());

    LobbyButton* enter = nullptr;
    for (const SavedGameSettings& set : std::as_const(m_saved))
    {
        const QString id = set.Id;
        auto* row = new QWidget(m_savedBox);
        row->setObjectName("SavedSettingsRow");
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);

        if (id == m_savedEditId && !m_savedEditDelete)
        {
            // Renaming, in place.
            auto* column = new QVBoxLayout;
            column->setSpacing(3);
            auto* edit = new QLineEdit(set.Name, row);
            edit->setObjectName("SavedSettingsRenameEdit");
            edit->setMaxLength(GameSettingsCatalog::MaxSavedNameLength);
            edit->setMinimumHeight(26);
            edit->setAccessibleName(tr("New name for %1").arg(set.Name));
            column->addWidget(edit);
            auto* error = smallText(QString(), "bad", row);
            error->setObjectName("SavedSettingsRenameError");
            error->setVisible(false);
            column->addWidget(error);
            lay->addLayout(column, 1);
            auto* save = rowButton(tr("Save"), LobbyButton::Kind::Primary, "SavedSettingsRenameSave", row);
            auto* back = rowButton(tr("Cancel"), LobbyButton::Kind::Ghost, "SavedSettingsRenameCancel", row);
            lay->addWidget(save, 0, Qt::AlignTop);
            lay->addWidget(back, 0, Qt::AlignTop);
            connect(save, &QPushButton::clicked, this, [this, id, edit, error]() { renameSaved(id, edit->text(), error); });
            connect(back, &QPushButton::clicked, this, [this]() {
                m_savedEditId.clear();
                rebuildSavedSection();
            });
            connect(edit, &QLineEdit::textChanged, error, &QWidget::hide);
            enter = save;
            QTimer::singleShot(0, edit, [edit]() {
                edit->setFocus();
                edit->selectAll();
            });
        }
        else if (id == m_savedEditId)
        {
            // Deleting, once asked.
            auto* ask = new ElidedLabel(LobbyTheme::Role::Ink, row);
            ask->setFont(LobbyTheme::uiFont(12));
            ask->setText(tr("Delete \"%1\"?").arg(set.Name));
            lay->addWidget(ask, 1);
            auto* yes = rowButton(tr("Delete"), LobbyButton::Kind::Danger, "SavedSettingsDeleteConfirm", row);
            yes->setAccessibleName(tr("Delete %1").arg(set.Name));
            auto* keep = rowButton(tr("Keep"), LobbyButton::Kind::Ghost, "SavedSettingsDeleteKeep", row);
            lay->addWidget(yes);
            lay->addWidget(keep);
            connect(yes, &QPushButton::clicked, this, [this, id]() { deleteSaved(id); });
            connect(keep, &QPushButton::clicked, this, [this]() {
                m_savedEditId.clear();
                rebuildSavedSection();
            });
            // Enter keeps it.
            enter = keep;
            QTimer::singleShot(0, keep, [keep]() { keep->setFocus(); });
        }
        else
        {
            const bool fits = set.Options.size() <= m_limit;
            auto* name = new ElidedLabel(set.Problem.isEmpty() ? LobbyTheme::Role::Muted : LobbyTheme::Role::Bad, row);
            name->setObjectName("SavedSettingsName");
            name->setFont(LobbyTheme::uiFont(12));
            name->setLead(set.Name);
            name->setText(QStringLiteral("  ·  ") +
                          (set.Problem.isEmpty() ? tr("%n options", nullptr, int(set.Options.size())) : set.Problem));
            name->setToolTip(set.Problem.isEmpty() ? m_catalog.labels(set.Options).join(QStringLiteral(", ")) : set.Problem);
            lay->addWidget(name, 1);
            auto* load = rowButton(tr("Load"), LobbyButton::Kind::Ghost, "SavedSettingsLoad", row);
            load->setAccessibleName(tr("Load %1").arg(set.Name));
            load->setEnabled(set.Problem.isEmpty() && fits);
            load->setToolTip(!set.Problem.isEmpty() ? set.Problem
                             : !fits ? tr("At most %1 game options can be on at once.").arg(m_limit)
                                     : tr("Turn these options on here."));
            auto* rename = rowButton(tr("Rename"), LobbyButton::Kind::Ghost, "SavedSettingsRename", row);
            rename->setAccessibleName(tr("Rename %1").arg(set.Name));
            auto* remove = rowButton(tr("Delete"), LobbyButton::Kind::Ghost, "SavedSettingsDelete", row);
            remove->setAccessibleName(tr("Delete %1").arg(set.Name));
            lay->addWidget(load);
            lay->addWidget(rename);
            lay->addWidget(remove);
            connect(load, &QPushButton::clicked, this, [this, id]() { loadSaved(id); });
            connect(rename, &QPushButton::clicked, this, [this, id]() {
                closeSaveBar();
                m_savedEditId = id;
                m_savedEditDelete = false;
                rebuildSavedSection();
            });
            connect(remove, &QPushButton::clicked, this, [this, id]() {
                closeSaveBar();
                m_savedEditId = id;
                m_savedEditDelete = true;
                rebuildSavedSection();
            });
        }
        m_savedRows->addWidget(row);
    }
    if (!m_savedEditId.isEmpty() && !GameSettingsCatalog::findSaved(m_saved, m_savedEditId))
        m_savedEditId.clear();
    setEnterButton(enter ? enter : !m_saveBar->isHidden() ? m_saveConfirm : m_mainButton);
}

void GameSettingsDialog::openSaveBar()
{
    if (!m_saveBar)
        return;
    if (!m_savedEditId.isEmpty())
    {
        m_savedEditId.clear();
        rebuildSavedSection();
    }
    m_saveBar->setVisible(true);
    if (m_editable)
    {
        m_saveName->clear();
    }
    else
    {
        m_note->setVisible(false);
        // Saved sets made elsewhere since count too.
        m_saved = m_catalog.savedSets(m_romMd5);
        refresh();
        // The host's name for their saved set, to keep or type over.
        m_saveName->setText(m_roomSavedByHost ? m_roomPresetLabel : QString());
    }
    checkSaveName();
    setEnterButton(m_saveConfirm);
    m_saveName->setFocus();
    m_saveName->selectAll();
}

void GameSettingsDialog::closeSaveBar()
{
    if (!m_saveBar || m_saveBar->isHidden())
        return;
    m_saveBar->setVisible(false);
    m_saveName->clear();
    setEnterButton(m_mainButton);
}

void GameSettingsDialog::checkSaveName()
{
    const QString blocker = saveBlocker();
    QString error;
    const QString name = m_catalog.savedNameFor(m_saveName->text(), &error);
    const SavedGameSettings* existing = nullptr;
    for (const SavedGameSettings& set : std::as_const(m_saved))
    {
        if (!name.isEmpty() && set.Name.compare(name, Qt::CaseInsensitive) == 0)
            existing = &set;
    }

    QString message;
    bool bad = false;
    bool ok = false;
    if (!blocker.isEmpty())
    {
        message = blocker;
    }
    else if (m_saveName->text().trimmed().isEmpty())
    {
        // Nothing typed yet.
    }
    else if (name.isEmpty())
    {
        message = error;
        bad = true;
    }
    else if (existing)
    {
        message = tr("You already have settings saved as \"%1\". Replace them with these?").arg(existing->Name);
        ok = true;
    }
    else if (m_saved.size() >= GameSettingsCatalog::MaxSavedSets)
    {
        message = tr("You can keep up to %1 saved settings for a game. Delete one to save another.")
                      .arg(GameSettingsCatalog::MaxSavedSets);
        bad = true;
    }
    else
    {
        ok = true;
    }

    const bool replace = ok && existing;
    m_saveConfirm->setText(replace ? tr("Replace") : tr("Save"));
    m_saveConfirm->setKind(replace ? LobbyButton::Kind::Danger : LobbyButton::Kind::Primary);
    m_saveConfirm->setEnabled(ok);
    setLabelRole(m_saveMessage, bad ? "bad" : "ink2");
    m_saveMessage->setText(message);
    m_saveMessage->setVisible(!message.isEmpty());
}

void GameSettingsDialog::saveAs()
{
    QString error;
    const QString name = m_catalog.savedNameFor(m_saveName->text(), &error);
    if (name.isEmpty() || !saveBlocker().isEmpty())
        return;
    int existing = -1;
    for (int i = 0; i < m_saved.size(); ++i)
    {
        if (m_saved.at(i).Name.compare(name, Qt::CaseInsensitive) == 0)
            existing = i;
    }
    if (existing < 0 && m_saved.size() >= GameSettingsCatalog::MaxSavedSets)
        return;
    if (existing >= 0)
    {
        // Same id, so a room or a remembered choice using it follows.
        m_saved[existing].Name = name;
        m_saved[existing].Options = m_on;
    }
    else
    {
        // A new id of this client's own, also for a copy of the host's saved set.
        m_saved.append({ GameSettingsCatalog::newSavedId(m_saved), name, m_on, QString() });
    }
    closeSaveBar();
    storeSaved(existing >= 0 ? tr("Replaced \"%1\" with these settings.").arg(name)
               : m_editable  ? tr("Saved as \"%1\". It's in the Game settings list now.").arg(name)
                             : tr("Saved as \"%1\". It's in your Game settings list when you host.").arg(name));
}

void GameSettingsDialog::renameSaved(const QString& id, const QString& text, QLabel* error)
{
    QString why;
    const QString name = m_catalog.savedNameFor(text, &why);
    for (const SavedGameSettings& set : std::as_const(m_saved))
    {
        if (!name.isEmpty() && set.Id != id && set.Name.compare(name, Qt::CaseInsensitive) == 0)
            why = tr("You already have settings saved as \"%1\".").arg(set.Name);
    }
    if (name.isEmpty() || !why.isEmpty())
    {
        error->setText(why);
        error->setVisible(true);
        return;
    }
    for (SavedGameSettings& set : m_saved)
    {
        if (set.Id != id)
            continue;
        const QString old = set.Name;
        set.Name = name;
        m_savedEditId.clear();
        storeSaved(old == name ? QString() : tr("Renamed \"%1\" to \"%2\".").arg(old, name));
        return;
    }
}

void GameSettingsDialog::deleteSaved(const QString& id)
{
    for (int i = 0; i < m_saved.size(); ++i)
    {
        if (m_saved.at(i).Id != id)
            continue;
        const QString name = m_saved.at(i).Name;
        m_saved.removeAt(i);
        m_savedEditId.clear();
        storeSaved(tr("Deleted \"%1\".").arg(name));
        return;
    }
}

void GameSettingsDialog::loadSaved(const QString& id)
{
    const SavedGameSettings* set = GameSettingsCatalog::findSaved(m_saved, id);
    if (!set || !set->Problem.isEmpty() || set->Options.size() > m_limit)
        return;
    m_on = m_catalog.ordered(set->Options);
    m_error->setVisible(false);
    m_note->setText(tr("Turned on what's saved as \"%1\".").arg(set->Name));
    m_note->setVisible(true);
    refresh();
}

void GameSettingsDialog::storeSaved(const QString& note)
{
    GameSettingsCatalog::storeSavedSets(m_romMd5, m_saved);
    // Read back, so the list is sorted and checked like everywhere else.
    m_saved = m_catalog.savedSets(m_romMd5);
    rebuildSavedSection();
    m_error->setVisible(false);
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
    refresh();
    emit savedSetsChanged();
}

void GameSettingsDialog::setEnterButton(LobbyButton* button)
{
    if (button)
        button->setDefault(true);
}

void GameSettingsDialog::keyPressEvent(QKeyEvent* event)
{
    // Escape leaves a name being typed, not the dialog.
    if (event->key() == Qt::Key_Escape && (!m_savedEditId.isEmpty() || (m_saveBar && !m_saveBar->isHidden())))
    {
        if (!m_savedEditId.isEmpty())
        {
            m_savedEditId.clear();
            rebuildSavedSection();
        }
        else
        {
            closeSaveBar();
        }
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

#endif // NETPLAY
