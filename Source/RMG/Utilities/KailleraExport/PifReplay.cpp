#include "PifReplay.hpp"

#include <cstring>

namespace KailleraExport
{

enum
{
    JCMD_STATUS = 0x00,
    JCMD_CONTROLLER_READ = 0x01,
    JCMD_PAK_READ = 0x02,
    JCMD_PAK_WRITE = 0x03,
    JCMD_RESET = 0xFF
};

static const KrecData* s_KrecData = nullptr;
static int s_InputFrameIndex = 0;
static bool s_SyncedThisFrame = false;
static bool s_ReplayFinished = false;
static uint32_t s_CachedInput[4] = {};
static int s_CachedNumPlayers = 0;

void InitializePifReplay(const KrecData* krecData)
{
    s_KrecData = krecData;
    s_InputFrameIndex = 0;
    s_SyncedThisFrame = false;
    s_ReplayFinished = false;
    s_CachedNumPlayers = 0;
    memset(s_CachedInput, 0, sizeof(s_CachedInput));
}

void ResetPifReplayFrameSync(void)
{
    s_SyncedThisFrame = false;
}

bool IsPifReplayFinished(void)
{
    return s_ReplayFinished;
}

int GetPifReplayFrameIndex(void)
{
    return s_InputFrameIndex;
}

void PifReplayCallback(struct pif* pifState)
{
    if (s_KrecData == nullptr || pifState == nullptr || s_ReplayFinished)
    {
        return;
    }

    int numPlayers = s_KrecData->header.numPlayers;
    if (numPlayers < 1)
    {
        numPlayers = 1;
    }
    if (numPlayers > 4)
    {
        numPlayers = 4;
    }

    const bool isControllerRead =
        (pifState->channels[0].tx != nullptr &&
         pifState->channels[0].tx_buf != nullptr &&
         pifState->channels[0].tx_buf[0] == JCMD_CONTROLLER_READ &&
         pifState->channels[0].rx_buf != nullptr);

    if (isControllerRead && !s_SyncedThisFrame)
    {
        s_SyncedThisFrame = true;

        const int bytesPerFrame = numPlayers * 4;
        const int offset = s_InputFrameIndex * bytesPerFrame;

        if (offset + bytesPerFrame <= static_cast<int>(s_KrecData->inputData.size()))
        {
            s_CachedNumPlayers = numPlayers;
            for (int i = 0; i < numPlayers; ++i)
            {
                const uint8_t* source = s_KrecData->inputData.data() + offset + i * 4;
                memcpy(&s_CachedInput[i], source, 4);
            }
            s_InputFrameIndex++;
        }
        else
        {
            s_ReplayFinished = true;
            s_CachedNumPlayers = 0;
            memset(s_CachedInput, 0, sizeof(s_CachedInput));
            return;
        }
    }

    for (int i = 0; i < numPlayers; ++i)
    {
        if (pifState->channels[i].tx == nullptr || pifState->channels[i].rx == nullptr ||
            pifState->channels[i].tx_buf == nullptr)
        {
            continue;
        }

        *pifState->channels[i].rx &= ~0xC0;

        const uint8_t command = pifState->channels[i].tx_buf[0];
        if (command == JCMD_STATUS || command == JCMD_RESET)
        {
            if (pifState->channels[i].rx_buf != nullptr)
            {
                const uint16_t type = 0x0500;
                pifState->channels[i].rx_buf[0] = static_cast<uint8_t>(type >> 0);
                pifState->channels[i].rx_buf[1] = static_cast<uint8_t>(type >> 8);
                pifState->channels[i].rx_buf[2] = 0;
            }
        }
        else if (command == JCMD_CONTROLLER_READ)
        {
            if (pifState->channels[i].rx_buf != nullptr && i < s_CachedNumPlayers)
            {
                pifState->channels[i].rx_buf[0] = static_cast<uint8_t>((s_CachedInput[i] >> 24) & 0xFF);
                pifState->channels[i].rx_buf[1] = static_cast<uint8_t>((s_CachedInput[i] >> 16) & 0xFF);
                pifState->channels[i].rx_buf[2] = static_cast<uint8_t>((s_CachedInput[i] >> 8) & 0xFF);
                pifState->channels[i].rx_buf[3] = static_cast<uint8_t>(s_CachedInput[i] & 0xFF);
            }
        }
        else if (command == JCMD_PAK_READ)
        {
            if (pifState->channels[i].rx_buf != nullptr)
            {
                pifState->channels[i].rx_buf[32] = 255;
            }
        }
        else if (command == JCMD_PAK_WRITE)
        {
            if (pifState->channels[i].rx_buf != nullptr)
            {
                pifState->channels[i].rx_buf[0] = 255;
            }
        }
    }
}

} // namespace KailleraExport
