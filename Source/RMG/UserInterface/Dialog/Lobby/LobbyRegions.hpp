#ifndef LOBBYREGIONS_HPP
#define LOBBYREGIONS_HPP

#include <QString>
#include <array>
#include <string_view>

namespace UserInterface
{
namespace Dialog
{
namespace LobbyRegions
{

struct Region
{
    std::string_view code;
    std::string_view label;
};

// Region codes must match what the server's HELLO handler accepts. Order
// here also drives the indexing into kRttMatrix below — keep them in sync.
inline constexpr std::array<Region, 9> kRegions = {{
    {"us-east",    "North America (East)"},
    {"us-central", "North America (Central)"},
    {"us-west",    "North America (West)"},
    {"sa",         "South America"},
    {"eu-west",    "Europe (West)"},
    {"eu-east",    "Europe (East)"},
    {"asia-east",  "Asia (East)"},
    {"asia-se",    "Asia (Southeast)"},
    {"oceania",    "Oceania"},
}};

// Symmetric round-trip-time estimate (ms) between each region pair. These
// are rough averages of typical residential internet routing — meant to give
// players a "is this match playable?" signal, not a precise measurement.
// Same-region diag is set to 30 to leave room for last-mile latency that's
// independent of geographic distance.
inline constexpr int kRttMatrix[9][9] = {
    //               usE  usC  usW   sa  euW  euE  asE  asSE oc
    /* us-east   */ { 30,  40,  80, 140,  90, 120, 180, 220, 200 },
    /* us-cent   */ { 40,  30,  50, 150, 110, 140, 170, 210, 190 },
    /* us-west   */ { 80,  50,  30, 170, 140, 170, 140, 180, 160 },
    /* sa        */ {140, 150, 170,  30, 180, 210, 240, 270, 220 },
    /* eu-west   */ { 90, 110, 140, 180,  30,  50, 220, 200, 280 },
    /* eu-east   */ {120, 140, 170, 210,  50,  30, 200, 180, 300 },
    /* asia-east */ {180, 170, 140, 240, 220, 200,  30,  60, 110 },
    /* asia-se   */ {220, 210, 180, 270, 200, 180,  60,  30, 100 },
    /* oceania   */ {200, 190, 160, 220, 280, 300, 110, 100,  30 },
};

inline int regionIndex(const QString& code)
{
    for (size_t i = 0; i < kRegions.size(); ++i)
    {
        if (code == QLatin1String(kRegions[i].code.data(), int(kRegions[i].code.size())))
            return int(i);
    }
    return -1;
}

inline QString labelFor(const QString& code)
{
    const int i = regionIndex(code);
    if (i < 0)
        return code; // unknown — show whatever the server sent verbatim
    return QLatin1String(kRegions[i].label.data(), int(kRegions[i].label.size()));
}

// Returns 0 when either code is unknown. Callers should treat 0 as "no estimate".
inline int estimatedRttMs(const QString& a, const QString& b)
{
    const int ia = regionIndex(a);
    const int ib = regionIndex(b);
    if (ia < 0 || ib < 0)
        return 0;
    return kRttMatrix[ia][ib];
}

// Default region for fresh installs. us-east is the most populated; better
// to land there than e.g. oceania and route everyone halfway around the
// world by accident.
inline constexpr const char* kDefaultRegion = "us-east";

} // namespace LobbyRegions
} // namespace Dialog
} // namespace UserInterface

#endif // LOBBYREGIONS_HPP
