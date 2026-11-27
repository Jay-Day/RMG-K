#ifdef NETPLAY

#include "GameSettingsDialog.hpp"

#include "CreateRoomDialog.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"
#include "StageListDialog.hpp"

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
        { QStringLiteral("GameOptionLists"), GameSettingsCatalog::listsToJson(Lists) },
    };
}

//
// StageListChoice
//

QString StageListChoice::data() const
{
    switch (Type)
    {
    case Kind::Preset:
        return QStringLiteral("preset:") + Id;
    case Kind::Saved:
        return QStringLiteral("saved:") + Id;
    case Kind::Custom:
        return QStringLiteral("custom");
    case Kind::Off:
        break;
    }
    return QStringLiteral("off");
}

QString StageListChoice::presetId() const
{
    switch (Type)
    {
    case Kind::Preset:
        return Id;
    case Kind::Saved:
        // Like a saved set's: other clients tell it from a preset list by it.
        return QStringLiteral("user-") + Id;
    case Kind::Custom:
    case Kind::Off:
        break;
    }
    return QString();
}

//
// GameSettingsCatalog
//

GameSettingsCatalog GameSettingsCatalog::forRom(const QString& romMd5)
{
    GameSettingsCatalog catalog;
    if (romMd5.isEmpty())
        return catalog;
    catalog.RomMd5 = romMd5;
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

const CoreGameOptionPresetInfo* GameSettingsCatalog::presetOrAlias(const QString& id) const
{
    if (const CoreGameOptionPresetInfo* info = preset(id))
        return info;
    const std::string key = id.toStdString();
    for (const CoreGameOptionPresetInfo& info : Presets)
    {
        if (std::find(info.Aliases.begin(), info.Aliases.end(), key) != info.Aliases.end())
            return &info;
    }
    return nullptr;
}

QStringList GameSettingsCatalog::extraOptionIds(const CoreGameOptionPresetInfo& preset, int players)
{
    QStringList ids;
    for (const CoreGameOptionPresetExtra& rule : preset.WithPlayers)
    {
        if (players < rule.MinPlayers)
            continue;
        for (const std::string& id : rule.Options)
            ids << QString::fromStdString(id);
    }
    return ids;
}

QStringList GameSettingsCatalog::matchOptions(const QStringList& on, const QString& presetId, int players) const
{
    const CoreGameOptionPresetInfo* info = presetId.isEmpty() ? nullptr : preset(presetId);
    if (!info || !sameSet(optionIds(*info), on))
        return on;
    const QStringList extra = extraOptionIds(*info, players);
    return extra.isEmpty() ? on : ordered(on + extra);
}

QStringList GameSettingsCatalog::blankSaveOptions(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids)
    {
        const CoreGameOptionInfo* info = option(id);
        if (info && info->NeedsBlankSave)
            out << id;
    }
    return out;
}

QString GameSettingsCatalog::withPlayersText(const CoreGameOptionPresetInfo& preset) const
{
    QStringList lines;
    const QString name = QString::fromStdString(preset.Label);
    for (const CoreGameOptionPresetExtra& rule : preset.WithPlayers)
    {
        // The pack's words for what the options do, else their names; hidden
        // ones are never named.
        QString what = QString::fromStdString(rule.Description);
        const bool described = !what.isEmpty();
        if (!described)
        {
            QStringList ids;
            for (const std::string& id : rule.Options)
                ids << QString::fromStdString(id);
            what = labels(visible(ids)).join(QStringLiteral(", "));
        }
        if (what.isEmpty())
            continue;
        // Rooms have at most 4 seats, so "4 or more" would read oddly.
        if (described)
            lines << (rule.MinPlayers >= 4
                          ? tr("With 4 players in the match, %1 %2.", "%2: what the preset does then").arg(name, what)
                          : tr("With %1 or more players in the match, %2 %3.", "%3: what the preset does then")
                                .arg(QString::number(rule.MinPlayers), name, what));
        else
            lines << (rule.MinPlayers >= 4 ? tr("With 4 players in the match, %1 also turns on %2.").arg(name, what)
                                           : tr("With %1 or more players in the match, %2 also turns on %3.")
                                                 .arg(QString::number(rule.MinPlayers), name, what));
    }
    return lines.join(QLatin1Char('\n'));
}

const CoreGameOptionPresetInfo* GameSettingsCatalog::presetFor(const QStringList& ids, const GameOptionLists& lists) const
{
    if (ids.isEmpty())
        return nullptr;
    QStringList sorted = ids;
    sorted.sort();
    sorted.removeDuplicates();
    const GameOptionLists wanted = listsFor(sorted, lists);
    for (const CoreGameOptionPresetInfo& info : Presets)
    {
        QStringList presetIds = optionIds(info);
        presetIds.sort();
        if (presetIds == sorted && presetLists(info) == wanted)
            return &info;
    }
    return nullptr;
}

GameOptionLists GameSettingsCatalog::presetLists(const CoreGameOptionPresetInfo& preset)
{
    GameOptionLists lists;
    for (const auto& [id, code] : preset.Lists)
        lists.insert(QString::fromStdString(id), QString::fromStdString(code));
    return lists;
}

bool GameSettingsCatalog::isHidden(const QString& id) const
{
    const CoreGameOptionInfo* info = option(id);
    return info && info->Hidden;
}

QStringList GameSettingsCatalog::visible(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids)
    {
        if (!isHidden(id))
            out << id;
    }
    return out;
}

QStringList GameSettingsCatalog::hidden(const QStringList& ids) const
{
    QStringList out;
    for (const QString& id : ids)
    {
        if (isHidden(id))
            out << id;
    }
    return out;
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

QStringList GameSettingsCatalog::fromStored(const QStringList& ids, GameOptionLists* lists) const
{
    // The pack says what an old id is now: Vanilla only is Vanilla CSS,
    // Dream Land only the Stage list on its Dream Land only list with Skip
    // stage select.
    QStringList mapped;
    for (const QString& id : ids)
    {
        if (option(id))
        {
            mapped << id;
            continue;
        }
        const std::string key = id.toStdString();
        for (const CoreGameOptionInfo& info : Options)
        {
            for (const CoreGameOptionAlias& alias : info.Aliases)
            {
                if (alias.Id != key)
                    continue;
                const QString to = QString::fromStdString(info.Id);
                mapped << to;
                for (const std::string& with : alias.With)
                    mapped << QString::fromStdString(with);
                // A list already remembered for the option stays.
                if (lists && !alias.StageListPreset.empty() && lists->value(to).isEmpty())
                    lists->insert(to, stageListPresetCode(to, QString::fromStdString(alias.StageListPreset)));
            }
        }
    }
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

QStringList GameSettingsCatalog::labels(const QStringList& ids, const GameOptionLists& lists) const
{
    QStringList out = labels(ids);
    for (int i = 0; i < ids.size(); ++i)
    {
        const QString list = takesList(ids.at(i)) ? stageListText(ids.at(i), lists.value(ids.at(i))) : QString();
        if (!list.isEmpty())
            out[i] = QStringLiteral("%1 (%2)").arg(out.at(i), list);
    }
    return out;
}

bool GameSettingsCatalog::takesList(const QString& id) const
{
    const CoreGameOptionInfo* info = option(id);
    return info && info->StageList;
}

bool GameSettingsCatalog::stageList(const QString& id, CoreStageListInfo& info) const
{
    if (!takesList(id) || Options.empty())
        return false;
    // Any of the options knows the ROM: every one of them is for it.
    return CoreGetStageList(RomMd5.toStdString(), id.toStdString(), language(), info);
}

int GameSettingsCatalog::stageCount(const QString& id, const QString& code) const
{
    CoreStageListInfo info;
    return stageList(id, info) ? int(stageListEntries(info, code).size()) : 0;
}

GameOptionLists GameSettingsCatalog::listsFor(const QStringList& ids, const GameOptionLists& lists) const
{
    GameOptionLists out;
    for (const QString& id : ids)
    {
        if (takesList(id) && !lists.value(id).isEmpty())
            out.insert(id, lists.value(id));
    }
    return out;
}

QString GameSettingsCatalog::listsProblem(const QStringList& ids, const GameOptionLists& lists) const
{
    for (const QString& id : ids)
    {
        if (!takesList(id))
            continue;
        const QString name = labels({ id }).value(0);
        if (lists.value(id).isEmpty())
            return tr("%1 has no stages picked.").arg(name);
        if (stageCount(id, lists.value(id)) == 0)
            return tr("%1's list can't be read by your game options pack.").arg(name);
    }
    return QString();
}

GameOptionLists GameSettingsCatalog::listsFromJson(const QString& json)
{
    GameOptionLists lists;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = o.begin(); it != o.end(); ++it)
    {
        if (it.value().isString() && !it.value().toString().isEmpty())
            lists.insert(it.key(), it.value().toString());
    }
    return lists;
}

QString GameSettingsCatalog::listsToJson(const GameOptionLists& lists)
{
    if (lists.isEmpty())
        return QString();
    QJsonObject o;
    for (auto it = lists.cbegin(); it != lists.cend(); ++it)
        o[it.key()] = it.value();
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QList<SavedStageList> GameSettingsCatalog::savedStageLists(const QString& id) const
{
    CoreStageListInfo info;
    return stageList(id, info) ? StageListDialog::savedLists(RomMd5, info) : QList<SavedStageList>();
}

QString GameSettingsCatalog::stageListPresetCode(const QString& id, const QString& presetId) const
{
    CoreStageListInfo info;
    const CoreStageListPreset* preset = stageList(id, info) ? info.preset(presetId.toStdString()) : nullptr;
    return preset ? QString::fromStdString(CoreEncodeStageList(info, preset->Stages)) : QString();
}

StageListChoice GameSettingsCatalog::stageListChoiceFor(const QString& id, const QString& code,
                                                        const QList<SavedStageList>& saved, const QString& savedHint) const
{
    StageListChoice choice;
    CoreStageListInfo info;
    const std::vector<CoreStageListEntry> entries = stageList(id, info) ? stageListEntries(info, code)
                                                                        : std::vector<CoreStageListEntry>();
    if (entries.empty())
        return choice;
    // Codes are canonical: the same list always has the same code.
    choice.Code = QString::fromStdString(CoreEncodeStageList(info, entries));
    for (const CoreStageListPreset& preset : info.Presets)
    {
        if (preset.Stages == entries)
        {
            choice.Type = StageListChoice::Kind::Preset;
            choice.Id = QString::fromStdString(preset.Id);
            return choice;
        }
    }
    const SavedStageList* match = nullptr;
    for (const SavedStageList& list : saved)
    {
        if ((!match || list.Id == savedHint) && stageListEntries(info, list.Code) == entries)
            match = &list;
    }
    choice.Type = match ? StageListChoice::Kind::Saved : StageListChoice::Kind::Custom;
    choice.Id = match ? match->Id : QString();
    return choice;
}

StageListChoice GameSettingsCatalog::stageListChoiceFromData(const QString& id, const QString& data,
                                                             const QList<SavedStageList>& saved) const
{
    StageListChoice choice;
    QString code;
    if (data.startsWith(QLatin1String("preset:")))
    {
        code = stageListPresetCode(id, data.mid(7));
    }
    else if (data.startsWith(QLatin1String("saved:")))
    {
        for (const SavedStageList& list : saved)
        {
            if (list.Id == data.mid(6))
                code = list.Code;
        }
    }
    if (code.isEmpty())
        return choice;
    choice = stageListChoiceFor(id, code, saved, data.startsWith(QLatin1String("saved:")) ? data.mid(6) : QString());
    return choice;
}

QString GameSettingsCatalog::stageListName(const QString& id, const StageListChoice& choice,
                                           const QList<SavedStageList>& saved) const
{
    if (choice.Type == StageListChoice::Kind::Preset)
    {
        CoreStageListInfo info;
        const CoreStageListPreset* preset = stageList(id, info) ? info.preset(choice.Id.toStdString()) : nullptr;
        return preset ? QString::fromStdString(preset->Label) : QString();
    }
    if (choice.Type == StageListChoice::Kind::Saved)
    {
        for (const SavedStageList& list : saved)
        {
            if (list.Id == choice.Id)
                return list.Name;
        }
    }
    return QString();
}

QString GameSettingsCatalog::stageListText(const QString& id, const QString& code) const
{
    const QList<SavedStageList> saved = savedStageLists(id);
    const StageListChoice choice = stageListChoiceFor(id, code, saved);
    if (choice.Type == StageListChoice::Kind::Off)
        return QString();
    const QString name = stageListName(id, choice, saved);
    return !name.isEmpty() ? name : stageCountText(stageCount(id, code));
}

QList<GameSettingsListItem> GameSettingsCatalog::stageListItems(const QString& id, const QList<SavedStageList>& saved,
                                                                const StageListChoice& current, int* currentIndex) const
{
    CoreStageListInfo info;
    stageList(id, info);
    QList<GameSettingsListItem> items;
    items.append({ tr("Off", "a stage list option that isn't on"), QStringLiteral("off"),
                   tr("The game's own stage select.") });
    for (const CoreStageListPreset& preset : info.Presets)
    {
        const QString tip = preset.Description.empty() ? stageListCountText(info, int(preset.Stages.size()))
                                                       : QString::fromStdString(preset.Description);
        items.append({ QString::fromStdString(preset.Label), QStringLiteral("preset:") + QString::fromStdString(preset.Id), tip });
    }
    // The host's own, set apart from the pack's.
    if (!saved.isEmpty())
    {
        items.append({ QString(), QString(), QString(), false, true });
        for (const SavedStageList& list : saved)
            items.append({ list.Name, QStringLiteral("saved:") + list.Id,
                           tr("Your saved list: %1").arg(stageListCountText(info, int(stageListEntries(info, list.Code).size()))) });
    }
    // A list made with Customize... that's none of the above, only while
    // it's the one picked.
    if (current.Type == StageListChoice::Kind::Custom)
    {
        const int stages = int(stageListEntries(info, current.Code).size());
        items.append({ QString(), QString(), QString(), false, true });
        items.append({ tr("Custom (%1)", "a stage list that isn't a preset or saved, with how many stages it has")
                           .arg(stageCountText(stages)),
                       QStringLiteral("custom"), stageListCountText(info, stages) });
    }
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
        GameOptionLists lists;
        const QJsonObject storedLists = o.value("lists").toObject();
        for (auto it = storedLists.begin(); it != storedLists.end(); ++it)
            lists.insert(it.key(), it.value().toString());
        // A set saved with an option that's a stage list now gets its list.
        set.Options = fromStored(stored, &lists);
        set.Lists = listsFor(set.Options, lists);
        set.Problem = set.Options.isEmpty() ? tr("None of its options are in your game options packs any more.")
                                            : problemWith(set.Options);
        if (set.Problem.isEmpty())
            set.Problem = listsProblem(set.Options, set.Lists);
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
        if (!set.Lists.isEmpty())
        {
            QJsonObject lists;
            for (auto it = set.Lists.cbegin(); it != set.Lists.cend(); ++it)
                lists[it.key()] = it.value();
            o["lists"] = lists;
        }
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

GameSettingsChoice GameSettingsCatalog::choiceFor(const QStringList& on, const GameOptionLists& lists,
                                                  const QList<SavedGameSettings>& saved, const QString& savedHint) const
{
    GameSettingsChoice choice;
    choice.Options = ordered(on);
    choice.Lists = listsFor(choice.Options, lists);
    if (choice.Options.isEmpty())
        return choice;
    // A preset with a stage list option has its own list too.
    if (const CoreGameOptionPresetInfo* info = presetFor(choice.Options, choice.Lists))
    {
        choice.Type = GameSettingsChoice::Kind::Preset;
        choice.Id = QString::fromStdString(info->Id);
        return choice;
    }
    const auto matches = [&](const SavedGameSettings& set) {
        return set.Problem.isEmpty() && sameSet(set.Options, choice.Options) && set.Lists == choice.Lists;
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
        return set && set->Problem.isEmpty() ? choiceFor(set->Options, set->Lists, saved, set->Id) : GameSettingsChoice();
    }
    if (kind == QLatin1String("preset"))
    {
        // By id, so it gets the options the pack gives it now. An id the pack
        // retired names the preset that took its place (1v1 Vanilla is
        // Vanilla Tournament now).
        if (const CoreGameOptionPresetInfo* info = presetOrAlias(s.value("GameSettingsPreset").toString()))
            return choiceFor(optionIds(*info), presetLists(*info), saved);
    }
    // Custom, or a room from before presets that only kept the options.
    GameOptionLists storedLists = listsFromJson(s.value("GameOptionLists").toString());
    const QStringList stored = fromStored(s.value("GameOptions").toStringList(), &storedLists);
    const GameOptionLists lists = listsFor(stored, storedLists);
    return problemWith(stored).isEmpty() && listsProblem(stored, lists).isEmpty() ? choiceFor(stored, lists, saved)
                                                                                  : GameSettingsChoice();
}

GameSettingsChoice GameSettingsCatalog::choiceFromData(const QString& data, const QList<SavedGameSettings>& saved,
                                                       const QStringList& custom, const GameOptionLists& customLists) const
{
    GameSettingsChoice choice;
    if (data.startsWith(QLatin1String("preset:")))
    {
        if (const CoreGameOptionPresetInfo* info = preset(data.mid(7)))
        {
            choice.Type = GameSettingsChoice::Kind::Preset;
            choice.Id = QString::fromStdString(info->Id);
            choice.Options = optionIds(*info);
            choice.Lists = presetLists(*info);
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
            choice.Lists = set->Lists;
        }
    }
    else if (data == QLatin1String("custom") && !custom.isEmpty())
    {
        choice.Type = GameSettingsChoice::Kind::Custom;
        choice.Options = ordered(custom);
        choice.Lists = listsFor(choice.Options, customLists);
    }
    return choice;
}

QList<GameSettingsListItem> GameSettingsCatalog::listItems(const QList<SavedGameSettings>& saved,
                                                           const GameSettingsChoice& current, int limit,
                                                           int* currentIndex) const
{
    const QString tooMany = tr("This lobby server takes at most %1 game options.").arg(limit);
    // A stage list says its name, or how many stages it has, as the room's
    // tag does.
    const auto withStages = [this](const QString& text, const GameOptionLists& lists) {
        QString out = text;
        for (auto it = lists.cbegin(); it != lists.cend(); ++it)
        {
            const QString list = stageListText(it.key(), it.value());
            if (!list.isEmpty())
                out += QStringLiteral(" · ") + list;
        }
        return out;
    };
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
                                      : tr("Your saved settings: %1").arg(labels(visible(set.Options), set.Lists).join(QStringLiteral(", ")));
            items.append({ withStages(set.Name, set.Lists), QStringLiteral("saved:") + set.Id, tip, set.Problem.isEmpty() && fits });
        }
    }
    // A set made with Customize... that's none of the above, only while it's
    // the one picked.
    if (current.Type == GameSettingsChoice::Kind::Custom)
    {
        items.append({ QString(), QString(), QString(), false, true });
        // Hidden options aren't counted or named.
        const QStringList shown = visible(current.Options);
        items.append({ withStages(tr("Custom (%1)", "game settings that aren't a preset, with how many options are on")
                                      .arg(shown.size()), current.Lists),
                       QStringLiteral("custom"), labels(shown, current.Lists).join(QStringLiteral(", ")) });
    }
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
                                       const GameOptionLists& lists, bool editable, int limit, QWidget* parent)
    : QDialog(parent), m_catalog(std::move(catalog)), m_editable(editable),
      m_limit(std::clamp(limit, 1, CoreGameOptionsMaxOptions)), m_gameName(gameName)
{
    setWindowTitle(tr("Game Settings"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setMinimumWidth(560);
    resize(600, 620);

    m_on = m_catalog.ordered(on);
    m_lists = m_catalog.listsFor(m_on, lists);
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
    // Hidden options get no row: what's on stays on (chosen()).
    const auto inGroup = [](const CoreGameOptionInfo& option, const std::string& group) {
        return option.Group == group && !option.Hidden;
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
        if (option.Hidden || std::find(listed.begin(), listed.end(), option.Group) != listed.end())
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
    m_playersRule = new QWidget(this);
    auto* ruleLay = new QHBoxLayout(m_playersRule);
    ruleLay->setContentsMargins(0, 0, 0, 0);
    ruleLay->setSpacing(5);
    ruleLay->addWidget(new IconLabel(QStringLiteral("user-add-line"), LobbyTheme::Role::Muted, 12, m_playersRule), 0,
                       Qt::AlignTop);
    m_playersRuleText = smallText(QString(), "muted", m_playersRule);
    m_playersRuleText->setObjectName("GameSettingsPlayersRule");
    ruleLay->addWidget(m_playersRuleText, 1);
    m_playersRule->setVisible(false);
    body->addWidget(m_playersRule);
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
    if (option.StageList)
    {
        addStageListRow(section, option);
        return;
    }
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
    addRelationLines(lay, row, option, false);

    section->addWidget(row);
    m_rows.push_back({ id, optionSwitch });
}

void GameSettingsDialog::addRelationLines(QVBoxLayout* lay, QWidget* row, const CoreGameOptionInfo& option, bool list)
{
    // What goes on or off with it, said where it can't be missed, in the
    // order the view shows them. Hidden options aren't named.
    const QString id = QString::fromStdString(option.Id);
    QStringList needs;
    for (const std::string& required : option.Requires)
        needs << QString::fromStdString(required);
    needs = m_catalog.visible(m_catalog.ordered(needs));
    QStringList conflicts;
    for (const std::string& conflict : option.Conflicts)
        conflicts << QString::fromStdString(conflict);
    conflicts = m_catalog.visible(m_catalog.ordered(conflicts));
    const QStringList neededBy = m_catalog.visible(m_catalog.ordered(m_neededBy.value(id)));
    if (!needs.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("add-line"), !m_editable ? tr("Needs %1.").arg(joinLabels(labelsOf(needs)))
            : list ? tr("Picking a list also turns on %1.").arg(joinLabels(labelsOf(needs)))
                   : tr("Turning this on also turns on %1.").arg(joinLabels(labelsOf(needs))), row));
    if (!neededBy.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("subtract-line"), !m_editable ? tr("Needed by %1.").arg(joinLabels(labelsOf(neededBy)))
            : list ? tr("Off also turns off %1.").arg(joinLabels(labelsOf(neededBy)))
                   : tr("Turning this off also turns off %1.").arg(joinLabels(labelsOf(neededBy))), row));
    if (!conflicts.isEmpty())
        lay->addWidget(relationLine(QStringLiteral("forbid-line"), !m_editable ? tr("Can't be on with %1.").arg(joinLabels(labelsOf(conflicts)))
            : list ? tr("Picking a list turns off %1.").arg(joinLabels(labelsOf(conflicts)))
                   : tr("Turning this on turns off %1.").arg(joinLabels(labelsOf(conflicts))), row));
    // Said here too, so the locked save picker in the room doesn't come as a surprise.
    if (option.NeedsBlankSave)
        lay->addWidget(relationLine(QStringLiteral("lock-line"),
            tr("Needs a blank save: while it's on, matches start without save data."), row));
}

void GameSettingsDialog::addStageListRow(QVBoxLayout* section, const CoreGameOptionInfo& option)
{
    // Shaped like the room's Game settings row: the option's name, a list of
    // named lists, and Customize... beside it.
    const QString id = QString::fromStdString(option.Id);
    const QString label = QString::fromStdString(option.Label);
    auto* row = new QWidget(section->parentWidget());
    row->setObjectName("GameSettingsStageListRow");
    auto* lay = new QVBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(3);

    StageRow stage;
    stage.Id = id;
    auto* line = new QHBoxLayout;
    line->setContentsMargins(kTextIndent, 0, 0, 0);
    line->setSpacing(8);
    auto* title = new QLabel(label, row);
    title->setObjectName("GameSettingsStageListTitle");
    title->setFont(LobbyTheme::uiFont(12));
    title->setProperty("role", "ink2");
    line->addWidget(title);
    stage.Combo = new QComboBox(row);
    stage.Combo->setObjectName("GameSettingsStageListCombo");
    stage.Combo->setProperty("gameOptionId", id);
    stage.Combo->setMinimumHeight(28);
    stage.Combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    stage.Combo->setMinimumContentsLength(12);
    stage.Combo->setAccessibleName(label);
    title->setBuddy(stage.Combo);
    line->addWidget(stage.Combo, 1);
    stage.Button = new LobbyButton(m_editable ? tr("Customize...") : tr("View..."), LobbyButton::Kind::Secondary, row);
    stage.Button->setObjectName("GameSettingsStagesButton");
    stage.Button->setIconName(m_editable ? QStringLiteral("edit-line") : QStringLiteral("eye-line"));
    stage.Button->setButtonSize(LobbyButton::Size::Small);
    stage.Button->setAccessibleName(m_editable ? tr("Customize %1").arg(label) : tr("View %1").arg(label));
    line->addWidget(stage.Button);
    lay->addLayout(line);

    if (!option.Description.empty())
    {
        auto* description = smallText(QString::fromStdString(option.Description), "muted", row);
        description->setContentsMargins(kTextIndent, 0, 0, 0);
        lay->addWidget(description);
    }
    addRelationLines(lay, row, option, true);

    // While it's on: the list's first icons and what's on it.
    stage.Box = new QWidget(row);
    stage.Box->setObjectName("GameSettingsStageRow");
    auto* boxLay = new QVBoxLayout(stage.Box);
    boxLay->setContentsMargins(kTextIndent, 4, 0, 2);
    boxLay->setSpacing(6);
    stage.Strip = new StageStrip(stage.Box);
    boxLay->addWidget(stage.Strip);
    stage.Summary = smallText(QString(), "ink2", stage.Box);
    stage.Summary->setObjectName("GameSettingsStageSummary");
    boxLay->addWidget(stage.Summary);
    lay->addWidget(stage.Box);

    if (m_editable)
        connect(stage.Combo, QOverload<int>::of(&QComboBox::activated), this, [this, id](int index) { pickStageList(id, index); });
    connect(stage.Button, &QPushButton::clicked, this, [this, id]() { openStageList(id); });
    section->addWidget(row);
    m_stageRows.push_back(stage);
}

QString GameSettingsDialog::roomListName(const QString& id) const
{
    const auto it = m_roomListNames.constFind(id);
    if (it == m_roomListNames.constEnd() || it->first.isEmpty())
        return QString();
    // One of this client's preset lists: its own label for it.
    if (!SavedGameSettings::isPresetId(it->first))
    {
        CoreStageListInfo info;
        if (m_catalog.stageList(id, info))
        {
            if (const CoreStageListPreset* preset = info.preset(it->first.toStdString()))
                return QString::fromStdString(preset->Label);
        }
    }
    return it->second;
}

void GameSettingsDialog::refreshStageRows()
{
    for (const StageRow& stage : m_stageRows)
    {
        const bool on = m_on.contains(stage.Id);
        CoreStageListInfo info;
        const bool known = m_catalog.stageList(stage.Id, info);
        const QString code = on ? m_lists.value(stage.Id) : QString();
        const std::vector<CoreStageListEntry> entries = known ? stageListEntries(info, code)
                                                              : std::vector<CoreStageListEntry>();

        // The list: every choice for the host, the room's for anyone else.
        const QList<SavedStageList> saved = known ? m_catalog.savedStageLists(stage.Id) : QList<SavedStageList>();
        const StageListChoice choice = m_catalog.stageListChoiceFor(stage.Id, code, saved);
        QList<GameSettingsListItem> items;
        int current = 0;
        if (m_editable)
        {
            items = m_catalog.stageListItems(stage.Id, saved, choice, &current);
        }
        else
        {
            // The host's name for it, else a preset list's with the same
            // stages, else how many it has.
            QString name = roomListName(stage.Id);
            if (name.isEmpty() && choice.Type == StageListChoice::Kind::Preset)
                name = m_catalog.stageListName(stage.Id, choice, saved);
            const QString text = !on ? tr("Off", "a stage list option that isn't on")
                               : !name.isEmpty() ? name
                               : entries.empty() ? tr("The host's list")
                                                 : tr("Custom (%1)", "a stage list that isn't a preset or saved, with how many stages it has")
                                                       .arg(stageCountText(int(entries.size())));
            items.append({ text, QString(), QString() });
        }
        GameSettingsCatalog::fillList(stage.Combo, items);
        {
            const QSignalBlocker blocker(stage.Combo);
            stage.Combo->setCurrentIndex(current);
        }
        // The lobby server would drop a list; an option already on can still
        // go off.
        const bool blocked = !m_listsUnavailable.isEmpty() && !on;
        stage.Combo->setEnabled(m_editable && known && !blocked);
        stage.Combo->setToolTip(blocked && m_editable ? m_listsUnavailable
                                : !m_editable && on && !entries.empty() ? stageListCountText(info, int(entries.size()))
                                                                        : QString());

        stage.Box->setVisible(on);
        const QHash<int, QImage>& icons = known ? stageListIcons(m_catalog.RomMd5, stage.Id, m_romPath) : QHash<int, QImage>();
        stage.Strip->setStages(icons, entries);
        stage.Strip->setVisible(!entries.empty() && !icons.isEmpty());
        QString text;
        if (entries.empty())
        {
            text = code.isEmpty() ? tr("No stages picked yet.") : tr("The host's list can't be read by your game options pack.");
        }
        else
        {
            QStringList names;
            for (size_t i = 0; i < entries.size() && i < 4; i++)
            {
                if (const CoreStageListStage* s = info.stage(entries[i].Stage))
                    names << QString::fromStdString(s->Name);
            }
            if (entries.size() > 4)
                names << tr("and %n more", nullptr, int(entries.size()) - 4);
            text = tr("%1: %2").arg(stageListCountText(info, int(entries.size())), names.join(QStringLiteral(", ")));
        }
        stage.Summary->setText(text);
        stage.Button->setEnabled(known && (m_editable ? m_listsUnavailable.isEmpty() : !entries.empty()));
        stage.Button->setToolTip(!m_listsUnavailable.isEmpty() && m_editable ? m_listsUnavailable
                                 : m_editable ? tr("Make a list of your own, or change this one, in the stage list editor.")
                                              : tr("See the host's list."));
    }
}

void GameSettingsDialog::pickStageList(const QString& id, int index)
{
    const StageRow* stage = nullptr;
    for (const StageRow& row : m_stageRows)
        stage = row.Id == id ? &row : stage;
    if (!stage || !m_editable)
        return;
    const QString data = stage->Combo->itemData(index).toString();
    m_error->setVisible(false);
    // "Custom (n stages)" is listed only while it's the choice: nothing to change.
    if (data == QLatin1String("custom"))
    {
        refresh();
        return;
    }
    if (data == QLatin1String("off"))
    {
        if (m_on.contains(id))
            toggle(id, false);
        else
            refresh();
        return;
    }
    const StageListChoice picked = m_catalog.stageListChoiceFromData(id, data, m_catalog.savedStageLists(id));
    if (picked.Type == StageListChoice::Kind::Off)
    {
        // Gone since the list was filled.
        refresh();
        return;
    }
    m_lists.insert(id, picked.Code);
    if (m_on.contains(id))
    {
        m_note->setVisible(false);
        refresh();
    }
    else
    {
        // On, with what that turns off.
        toggle(id, true);
    }
}

void GameSettingsDialog::openStageList(const QString& id)
{
    if (m_stageDialog)
        return;
    CoreStageListInfo info;
    if (!m_catalog.stageList(id, info))
        return;
    // The host picks up where the option was, even after turning it off here.
    const QString code = m_editable || m_on.contains(id) ? m_lists.value(id) : QString();
    auto* dialog = new StageListDialog(m_gameName, std::move(info), code, m_editable, m_catalog.RomMd5, m_romPath, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (!m_editable)
        dialog->setRoomListName(roomListName(id), SavedGameSettings::isPresetId(m_roomListNames.value(id).first));
    m_stageDialog = dialog;
    m_stageDialogId = id;
    // Lists saved, renamed or deleted there show in the list here, and in
    // the room's.
    connect(dialog, &StageListDialog::savedListsChanged, this, [this]() {
        refresh();
        emit savedSetsChanged();
    });
    connect(dialog, &QDialog::finished, this, [this, dialog, id](int result) {
        if (!m_editable || result != QDialog::Accepted)
        {
            refresh();
            return;
        }
        const QString made = dialog->code();
        m_error->setVisible(false);
        if (!made.isEmpty())
        {
            m_lists.insert(id, made);
            if (!m_on.contains(id))
            {
                toggle(id, true);
                return;
            }
            m_note->setVisible(false);
        }
        else if (m_on.contains(id))
        {
            // No stages: the option can't be on.
            m_lists.remove(id);
            m_on.removeAll(id);
            m_note->setText(tr("Turned %1 off: its list has no stages.").arg(labelOf(id)));
            m_note->setVisible(true);
        }
        refresh();
    });
    dialog->open();
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
    if (on && m_catalog.takesList(id) && !m_listsUnavailable.isEmpty())
    {
        m_note->setText(m_listsUnavailable);
        m_note->setVisible(true);
        refresh();
        return;
    }
    // A stage list option needs its stages: without a list it stays off, and
    // the editor opens to make one.
    if (on && m_catalog.takesList(id) && m_lists.value(id).isEmpty())
    {
        QTimer::singleShot(0, this, [this, id]() { openStageList(id); });
        refresh();
        return;
    }
    m_on = m_catalog.ordered(next);

    // Hidden options go on and off with the others without a word.
    turnedOn = m_catalog.visible(turnedOn);
    offConflicting = m_catalog.visible(offConflicting);
    offDependent = m_catalog.visible(offDependent);
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
        // The lobby server would drop a stage list.
        const bool blocked = !on && m_catalog.takesList(row.Id) && !m_listsUnavailable.isEmpty();
        row.Switch->setEnabled(fits && !blocked);
        row.Switch->setToolTip(blocked ? m_listsUnavailable
                               : fits  ? QString()
                                       : tr("At most %1 game options can be on at once.").arg(m_limit));
    }

    refreshStageRows();

    // Options on that the packs don't know count too (read-only view only);
    // hidden ones don't, but they take their room in the limit.
    const int otherCount = int(m_otherLabels.size());
    const int hiddenCount = int(m_catalog.hidden(m_on).size());
    const int count = int(m_on.size()) - hiddenCount + otherCount;
    m_count->setText(m_editable ? tr("%1 of %2 on").arg(count).arg(std::max(0, m_limit - hiddenCount))
                                : tr("%n on", nullptr, count));
    m_count->setToolTip(m_limitNote);

    const CoreGameOptionPresetInfo* preset = otherCount == 0 ? m_catalog.presetFor(m_on, m_lists) : nullptr;
    const SavedGameSettings* saved = otherCount == 0 && !preset ? savedMatching(m_on, m_lists) : nullptr;
    QString matches;
    if (m_on.isEmpty() && otherCount == 0)
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
    // A preset can add options in bigger matches (Teams at 4 players), which
    // the switches don't show.
    const QString playersRule = preset ? m_catalog.withPlayersText(*preset) : QString();
    m_playersRuleText->setText(playersRule);
    m_playersRule->setVisible(!playersRule.isEmpty());

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
        QString problem = m_catalog.problemWith(m_on);
        if (problem.isEmpty())
            problem = m_catalog.listsProblem(m_on, m_lists);
        if (!problem.isEmpty())
            return tr("Your game options packs don't allow these settings: %1").arg(problem);
    }
    if (m_on.isEmpty())
        return tr("Turn some options on to save them.");
    if (const QString problem = m_catalog.listsProblem(m_on, m_lists); !problem.isEmpty())
        return problem;
    if (const CoreGameOptionPresetInfo* preset = m_catalog.presetFor(m_on, m_lists))
        return tr("These are the %1 preset's settings.").arg(QString::fromStdString(preset->Label));
    if (const SavedGameSettings* saved = savedMatching(m_on, m_lists))
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

void GameSettingsDialog::setRomPath(const QString& romPath)
{
    m_romPath = romPath;
    refresh();
}

void GameSettingsDialog::setListsUnavailable(const QString& why)
{
    m_listsUnavailable = why;
    for (const Row& row : m_rows)
    {
        if (m_catalog.takesList(row.Id) && !why.isEmpty())
            row.Switch->setToolTip(why);
    }
    refresh();
}

void GameSettingsDialog::showRoomOptions(const QList<LobbyClient::LobbyGameOption>& on, const QString& presetLabel,
                                         bool savedByHost)
{
    QStringList ids, known, otherLabels;
    GameOptionLists lists;
    QMap<QString, QPair<QString, QString>> listNames;
    QList<LobbyClient::LobbyGameOption> unknown;
    for (const LobbyClient::LobbyGameOption& option : on)
    {
        ids << option.id;
        if (m_catalog.option(option.id))
        {
            known << option.id;
            if (!option.param.isEmpty())
                lists.insert(option.id, option.param);
            if (!option.paramPresetId.isEmpty())
                listNames.insert(option.id, { option.paramPresetId, option.paramPresetLabel });
        }
        else
        {
            unknown << option;
            otherLabels << option.label;
        }
    }
    lists = m_catalog.listsFor(known, lists);
    // The room's list is sent again for every room update; only a change
    // matters here.
    if (m_roomShown && ids == m_roomIds && otherLabels == m_otherLabels && presetLabel == m_roomPresetLabel &&
        savedByHost == m_roomSavedByHost && lists == m_lists && listNames == m_roomListNames)
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
    m_roomListNames = listNames;
    m_on = m_catalog.ordered(known);
    m_lists = lists;
    // An open view of the host's list follows it, with its name.
    if (auto* view = qobject_cast<StageListDialog*>(m_stageDialog.data()))
    {
        view->setRoomListName(roomListName(m_stageDialogId),
                              SavedGameSettings::isPresetId(m_roomListNames.value(m_stageDialogId).first));
        view->showCode(m_lists.value(m_stageDialogId));
    }

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

void GameSettingsDialog::setApplyCheck(std::function<bool(const QStringList&, const GameOptionLists&, QString&)> check)
{
    m_check = std::move(check);
}

QStringList GameSettingsDialog::chosen() const
{
    return m_on;
}

GameOptionLists GameSettingsDialog::chosenLists() const
{
    return m_catalog.listsFor(m_on, m_lists);
}

void GameSettingsDialog::apply()
{
    QString error = m_catalog.listsProblem(chosen(), chosenLists());
    if (!error.isEmpty())
    {
        m_error->setText(error);
        m_error->setVisible(true);
        return;
    }
    if (m_check && !m_check(chosen(), chosenLists(), error))
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

const SavedGameSettings* GameSettingsDialog::savedMatching(const QStringList& ids, const GameOptionLists& lists) const
{
    if (ids.isEmpty())
        return nullptr;
    const GameOptionLists wanted = m_catalog.listsFor(ids, lists);
    for (const SavedGameSettings& set : m_saved)
    {
        if (set.Problem.isEmpty() && sameSet(set.Options, ids) && set.Lists == wanted)
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
                          (set.Problem.isEmpty() ? tr("%n options", nullptr, int(m_catalog.visible(set.Options).size())) : set.Problem));
            name->setToolTip(set.Problem.isEmpty() ? m_catalog.labels(m_catalog.visible(set.Options), set.Lists).join(QStringLiteral(", "))
                                                   : set.Problem);
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
    // A stage list goes with the set.
    const GameOptionLists lists = m_catalog.listsFor(m_on, m_lists);
    if (existing >= 0)
    {
        // Same id, so a room or a remembered choice using it follows.
        m_saved[existing].Name = name;
        m_saved[existing].Options = m_on;
        m_saved[existing].Lists = lists;
    }
    else
    {
        // A new id of this client's own, also for a copy of the host's saved set.
        m_saved.append({ GameSettingsCatalog::newSavedId(m_saved), name, m_on, QString(), lists });
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
    // Its stage lists replace the ones here; others stay for when they're on again.
    for (auto it = set->Lists.cbegin(); it != set->Lists.cend(); ++it)
        m_lists.insert(it.key(), it.value());
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
