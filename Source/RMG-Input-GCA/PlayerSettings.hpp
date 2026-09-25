#ifndef GCA_PLAYER_SETTINGS_HPP
#define GCA_PLAYER_SETTINGS_HPP

#include <RMG-Core/Settings.hpp>
#include <string>
#include <algorithm>

// Profiles follow the emulated player, independently of adapter-port routing.
inline std::string GameCubePlayerSection(int player)
{
    return "Input-GCA Player " + std::to_string(player + 1);
}

inline std::string GameCubeNamedProfileSection(const std::string& name)
{
    return "Input-GCA Profile \"" + name + "\"";
}

inline std::string GameCubeEffectiveSection(int player)
{
    const auto base = GameCubePlayerSection(player);
    if (!CoreSettingsSectionExists(base)) return base;
    const auto name = CoreSettingsGetStringValue(SettingsID::GCAInput_UseProfile, base);
    const auto names = CoreSettingsGetStringListValue(SettingsID::GCAInput_Profiles);
    const auto named = GameCubeNamedProfileSection(name);
    if (!name.empty() && std::find(names.begin(), names.end(), name) != names.end() && CoreSettingsSectionExists(named)) return named;
    return base;
}

// Until a player is saved, retain the legacy shared configuration.
inline int ReadGameCubePlayerInt(SettingsID setting, int player)
{
    const auto section = GameCubeEffectiveSection(player);
    return CoreSettingsSectionExists(section) ? CoreSettingsGetIntValue(setting, section) : CoreSettingsGetIntValue(setting);
}

inline bool ReadGameCubePlayerBool(SettingsID setting, int player)
{
    const auto section = GameCubeEffectiveSection(player);
    return CoreSettingsSectionExists(section) ? CoreSettingsGetBoolValue(setting, section) : CoreSettingsGetBoolValue(setting);
}

#endif
