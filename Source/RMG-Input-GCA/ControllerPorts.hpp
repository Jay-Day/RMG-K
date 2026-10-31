#ifndef GCA_CONTROLLERPORTS_HPP
#define GCA_CONTROLLERPORTS_HPP

#include <array>
#include <cstddef>
#include <vector>

inline std::array<int, 4> ResolveGameCubeControllerPorts(
    const std::vector<int>& configured, const std::array<bool, 4>& enabled)
{
    std::array<int, 4> result = {-1, -1, -1, -1};
    if (configured.size() != result.size())
    {
        std::size_t slot = 0;
        for (std::size_t port = 0; port < enabled.size(); ++port)
            if (enabled[port]) result[slot++] = static_cast<int>(port);
        return result;
    }

    std::array<bool, 4> used{};
    for (std::size_t slot = 0; slot < result.size(); ++slot)
    {
        const int port = configured[slot];
        if (port >= 0 && port < 4 && !used[static_cast<std::size_t>(port)])
        {
            result[slot] = port;
            used[static_cast<std::size_t>(port)] = true;
        }
    }
    return result;
}

#endif
