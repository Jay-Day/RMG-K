#ifndef GCA_PLAYER_SETTINGS_HPP
#define GCA_PLAYER_SETTINGS_HPP

#include <RMG-Core/Settings.hpp>
#include <string>

// Profiles follow the emulated player, independently of adapter-port routing.
inline std::string GameCubePlayerSection(int player)
{
    return "Input-GCA Player " + std::to_string(player + 1);
}

// Until a player is saved, retain the legacy shared configuration.
inline int ReadGameCubePlayerInt(SettingsID setting, int player)
{
    const auto section = GameCubePlayerSection(player);
    return CoreSettingsSectionExists(section) ? CoreSettingsGetIntValue(setting, section) : CoreSettingsGetIntValue(setting);
}

inline bool ReadGameCubePlayerBool(SettingsID setting, int player)
{
    const auto section = GameCubePlayerSection(player);
    return CoreSettingsSectionExists(section) ? CoreSettingsGetBoolValue(setting, section) : CoreSettingsGetBoolValue(setting);
}

#endif
