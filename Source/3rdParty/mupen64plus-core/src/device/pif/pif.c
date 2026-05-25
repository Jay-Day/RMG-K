/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - pif.c                                                   *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "pif.h"
#include "n64_cic_nus_6105.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "api/callbacks.h"
#include "api/m64p_plugin.h"
#include "api/m64p_types.h"
#include "backends/api/joybus.h"
#include "device/memory/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/si/si_controller.h"
#include "plugin/plugin.h"
#include "main/netplay.h"
#include "main/pif_sync_callback.h"
#include "osal/files.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

//#define DEBUG_PIF
#ifdef DEBUG_PIF
void print_pif(struct pif* pif)
{
    int i;
    for (i=0; i<(64/8); i++) {
        DebugMessage(M64MSG_INFO, "%02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " | %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8,
                     pif->ram[i*8+0], pif->ram[i*8+1],pif->ram[i*8+2], pif->ram[i*8+3],
                     pif->ram[i*8+4], pif->ram[i*8+5],pif->ram[i*8+6], pif->ram[i*8+7]);
    }

    for(i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        if (pif->channels[i].tx != NULL) {
            DebugMessage(M64MSG_INFO, "Channel %u, tx=%02x rx=%02x cmd=%02x",
                i,
                *(pif->channels[i].tx),
                *(pif->channels[i].rx),
                pif->channels[i].tx_buf[0]);
        }
    }
}
#endif

enum { ROLLBACK_INPUT_PLAYERS = 4 };

static m64p_rollback_input_callback l_rollback_input_callback = NULL;
static uint32_t l_rollback_input_values[ROLLBACK_INPUT_PLAYERS];
static int l_rollback_input_valid = 0;
static int l_rollback_input_players = 0;

static uint32_t rollback_read_controller_input(const uint8_t* rx_buf)
{
    return ((uint32_t)rx_buf[0] << 24)
        | ((uint32_t)rx_buf[1] << 16)
        | ((uint32_t)rx_buf[2] << 8)
        | (uint32_t)rx_buf[3];
}

static void rollback_write_controller_input(uint8_t* rx_buf, uint32_t value)
{
    rx_buf[0] = (uint8_t)((value >> 24) & 0xff);
    rx_buf[1] = (uint8_t)((value >> 16) & 0xff);
    rx_buf[2] = (uint8_t)((value >> 8) & 0xff);
    rx_buf[3] = (uint8_t)(value & 0xff);
}

static int rollback_channel_has_command(const struct pif_channel* channel)
{
    return channel->tx != NULL
        && channel->rx != NULL
        && channel->tx_buf != NULL
        && channel->rx_buf != NULL;
}

static int rollback_verbose_pif_input_logging_enabled(void)
{
    const char* value = getenv("RMGK_VERBOSE_PIF_INPUT_LOGGING");
    return value != NULL && value[0] == '1';
}

static const char* rollback_log_path_separator(const char* directory)
{
    size_t length;
    char last;

    if (directory == NULL) {
        return "";
    }

    length = strlen(directory);
    if (length == 0) {
        return "";
    }

    last = directory[length - 1];
    return (last == '/' || last == '\\') ? "" : "/";
}

static FILE* rollback_open_log_file(const char* suffix)
{
    const char* directory = getenv("RMGK_ROLLBACK_LOG_DIR");
    const char* prefix = getenv("RMGK_ROLLBACK_LOG_PREFIX");
    char path[PATH_MAX];
    int written;
    FILE* file;

    if (directory != NULL && directory[0] != '\0' && prefix != NULL && prefix[0] != '\0') {
        written = snprintf(path, sizeof(path), "%s%s%s_%s.log",
            directory,
            rollback_log_path_separator(directory),
            prefix,
            suffix);
        if (written > 0 && (size_t)written < sizeof(path)) {
            file = fopen(path, "a");
            if (file != NULL) {
                return file;
            }
        }
    }

    osal_mkdirp("Logs", 0700);
    written = snprintf(path, sizeof(path), "Logs%srollback_%s.log", rollback_log_path_separator("Logs"), suffix);
    if (written > 0 && (size_t)written < sizeof(path)) {
        file = fopen(path, "a");
        if (file != NULL) {
            return file;
        }
    }

    osal_mkdirp("Bin/Release/Logs", 0700);
    written = snprintf(path, sizeof(path), "Bin/Release/Logs%srollback_%s.log",
        rollback_log_path_separator("Bin/Release/Logs"),
        suffix);
    if (written > 0 && (size_t)written < sizeof(path)) {
        return fopen(path, "a");
    }

    return NULL;
}

static void rollback_log_pif_channel(const char* phase, size_t index, const struct pif_channel* channel)
{
    FILE* file;

    if (!rollback_verbose_pif_input_logging_enabled() || l_rollback_input_callback == NULL || l_rollback_input_players <= 0 || !rollback_channel_has_command(channel)) {
        return;
    }

    file = rollback_open_log_file("pif");
    if (file == NULL) {
        return;
    }

    fprintf(file,
        "phase=%s ch=%u tx=0x%02x rx=0x%02x cmd=0x%02x ijbd=%d rx0=0x%02x rx1=0x%02x rx2=0x%02x rx3=0x%02x rx32=0x%02x\n",
        phase,
        (unsigned)index,
        (unsigned)*channel->tx,
        (unsigned)*channel->rx,
        (unsigned)channel->tx_buf[0],
        channel->ijbd != NULL,
        (unsigned)channel->rx_buf[0],
        (unsigned)channel->rx_buf[1],
        (unsigned)channel->rx_buf[2],
        (unsigned)channel->rx_buf[3],
        (unsigned)channel->rx_buf[32]);

    fclose(file);
}

static void rollback_force_controller_present(struct pif_channel* channel)
{
    if (!rollback_channel_has_command(channel)) {
        return;
    }

    switch (channel->tx_buf[0])
    {
    case JCMD_STATUS:
    case JCMD_RESET:
        *channel->rx &= (uint8_t)~0xc0;
        channel->rx_buf[0] = 0x05;
        channel->rx_buf[1] = 0x00;
        channel->rx_buf[2] = 0x00;
        break;
    case JCMD_CONTROLLER_READ:
        *channel->rx &= (uint8_t)~0xc0;
        channel->rx_buf[0] = 0x00;
        channel->rx_buf[1] = 0x00;
        channel->rx_buf[2] = 0x00;
        channel->rx_buf[3] = 0x00;
        break;
    case JCMD_PAK_READ:
        *channel->rx &= (uint8_t)~0xc0;
        if (channel->ijbd == NULL) {
            channel->rx_buf[32] = 0xff;
        }
        break;
    case JCMD_PAK_WRITE:
        *channel->rx &= (uint8_t)~0xc0;
        if (channel->ijbd == NULL) {
            channel->rx_buf[0] = 0xff;
        }
        break;
    default:
        break;
    }
}

static int rollback_should_skip_raw_pif_channel(size_t player)
{
    return l_rollback_input_callback != NULL
        && player < (size_t)l_rollback_input_players
        && player < NUM_CONTROLLER
        && Controls[player].RawData;
}

void pif_begin_rollback_input_frame(void)
{
    l_rollback_input_valid = 0;
}

void pif_set_rollback_input_callback(m64p_rollback_input_callback callback)
{
    l_rollback_input_callback = callback;
    l_rollback_input_valid = 0;
    if (callback == NULL) {
        l_rollback_input_players = 0;
    }
}

void pif_set_rollback_input_players(int players)
{
    if (players < 0) {
        players = 0;
    }
    if (players > ROLLBACK_INPUT_PLAYERS) {
        players = ROLLBACK_INPUT_PLAYERS;
    }

    l_rollback_input_players = players;
    l_rollback_input_valid = 0;
}

static void rollback_sync_input(struct pif* pif)
{
    uint32_t input_values[ROLLBACK_INPUT_PLAYERS] = { 0 };
    int has_controller_read = 0;
    size_t k;

    if (l_rollback_input_callback == NULL) {
        return;
    }

    for (k = 0; k < (size_t)l_rollback_input_players && k < PIF_CHANNELS_COUNT; ++k) {
        rollback_force_controller_present(&pif->channels[k]);
    }

    if (l_rollback_input_valid) {
        for (k = 0; k < (size_t)l_rollback_input_players && k < PIF_CHANNELS_COUNT; ++k) {
            struct pif_channel* channel = &pif->channels[k];

            if (rollback_channel_has_command(channel)
            && channel->tx_buf[0] == JCMD_CONTROLLER_READ) {
                *channel->rx &= (uint8_t)~0xc0;
                rollback_write_controller_input(channel->rx_buf, l_rollback_input_values[k]);
            }
        }
        return;
    }

    for (k = 0; k < (size_t)l_rollback_input_players && k < PIF_CHANNELS_COUNT; ++k) {
        struct pif_channel* channel = &pif->channels[k];

        if (rollback_channel_has_command(channel)
        && channel->tx_buf[0] == JCMD_CONTROLLER_READ
        && ((*channel->rx & 0x80) == 0)) {
            has_controller_read = 1;
            input_values[k] = rollback_read_controller_input(channel->rx_buf);
        }
    }

    if (!has_controller_read) {
        return;
    }

    if (!l_rollback_input_callback(input_values, sizeof(input_values[0]), l_rollback_input_players)) {
        return;
    }

    memcpy(l_rollback_input_values, input_values, sizeof(l_rollback_input_values));
    l_rollback_input_valid = 1;

    for (k = 0; k < (size_t)l_rollback_input_players && k < PIF_CHANNELS_COUNT; ++k) {
        struct pif_channel* channel = &pif->channels[k];

        if (rollback_channel_has_command(channel)
        && channel->tx_buf[0] == JCMD_CONTROLLER_READ) {
            *channel->rx &= (uint8_t)~0xc0;
            rollback_write_controller_input(channel->rx_buf, input_values[k]);
        }
    }
}

static void process_channel(struct pif_channel* channel)
{
    /* don't process channel if it has been disabled */
    if (channel->tx == NULL) {
        return;
    }

    /* reset Tx/Rx just in case */
    *channel->tx &= 0x3f;
    *channel->rx &= 0x3f;

    /* set NoResponse if no device is connected */
    if (channel->ijbd == NULL) {
        *channel->rx |= 0x80;
        return;
    }

    /* do device processing */
    channel->ijbd->process(channel->jbd,
        channel->tx, channel->tx_buf,
        channel->rx, channel->rx_buf);
}

static void post_setup_channel(struct pif_channel* channel)
{
    if ((channel->ijbd == NULL)
    || (channel->ijbd->post_setup == NULL)) {
        return;
    }

    channel->ijbd->post_setup(channel->jbd,
        channel->tx, channel->tx_buf,
        channel->rx, channel->rx_buf);
}

void disable_pif_channel(struct pif_channel* channel)
{
    channel->tx = NULL;
    channel->rx = NULL;
    channel->tx_buf = NULL;
    channel->rx_buf = NULL;
}

size_t setup_pif_channel(struct pif_channel* channel, uint8_t* buf)
{
    uint8_t tx = buf[0] & 0x3f;
    uint8_t rx = buf[1] & 0x3f;

    /* XXX: check out of bounds accesses */

    channel->tx = buf;
    channel->rx = buf + 1;
    channel->tx_buf = buf + 2;
    channel->rx_buf = buf + 2 + tx;

    post_setup_channel(channel);

    return 2 + tx + rx;
}

void init_pif(struct pif* pif,
    uint8_t* pif_base,
    void* jbds[PIF_CHANNELS_COUNT],
    const struct joybus_device_interface* ijbds[PIF_CHANNELS_COUNT],
    const uint8_t* ipl3,
    struct r4300_core* r4300,
    struct si_controller* si)
{
    size_t i;

    pif->base = pif_base;
    pif->ram = pif_base + 0x7c0;

    for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        pif->channels[i].jbd = jbds[i];
        pif->channels[i].ijbd = ijbds[i];
    }

    init_cic_using_ipl3(&pif->cic, ipl3);

    pif->r4300 = r4300;
    pif->si = si;
}

void reset_pif(struct pif* pif, unsigned int reset_type)
{
    size_t i;

    /* HACK: for allowing pifbootrom execution */
    unsigned int rom_type = (pif->cic.version == CIC_8303 || pif->cic.version == CIC_8401 || pif->cic.version == CIC_8501) ? 1 : 0;
    unsigned int s7 = 0;

    /* 0:ColdReset, 1:NMI */
    assert((reset_type & ~0x1) == 0);

    /* disable channel processing */
    for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
        disable_pif_channel(&pif->channels[i]);
    }

    /* set PIF_24 with reset informations */
    uint32_t* pif24 = (uint32_t*)(pif->ram + 0x24);
    *pif24 = (uint32_t)
         (((rom_type      & 0x1) << 19)
        | ((s7            & 0x1) << 18)
        | ((reset_type    & 0x1) << 17)
        | ((pif->cic.seed & 0xff) << 8)
        | 0x3f);
    *pif24 = fromhl(*pif24);

    /* clear PIF flags */
    pif->ram[0x3f] = 0x00;
}

void setup_channels_format(struct pif* pif)
{
    size_t i = 0;
    size_t k = 0;

    while (i < PIF_RAM_SIZE && k < PIF_CHANNELS_COUNT)
    {
        switch(pif->ram[i])
        {
        case 0x00: /* skip channel */
            disable_pif_channel(&pif->channels[k++]);
            ++i;
            break;

        case 0xff: /* dummy data */
            ++i;
            break;

        case 0xfe: /* end of channel setup - remaining channels are disabled */
            while (k < PIF_CHANNELS_COUNT) {
                disable_pif_channel(&pif->channels[k++]);
            }
            break;

        case 0xfd: /* channel reset - send reset command and discard the results */ {
            static uint8_t dummy_reset_buffer[PIF_CHANNELS_COUNT][6];

            /* setup reset command Tx=1, Rx=3, cmd=0xff */
            dummy_reset_buffer[k][0] = 0x01;
            dummy_reset_buffer[k][1] = 0x03;
            dummy_reset_buffer[k][2] = 0xff;

            setup_pif_channel(&pif->channels[k], dummy_reset_buffer[k]);
            ++k;
            ++i;
            }
            break;

        default: /* setup channel */

            /* HACK?: some games sends bogus PIF commands while accessing controller paks
             * Yoshi Story, Top Gear Rally 2, Indiana Jones, ...
             * When encountering such commands, we skip this bogus byte.
             */
            if ((i+1 < PIF_RAM_SIZE) && (pif->ram[i+1] == 0xfe)) {
                ++i;
                continue;
            }

            if ((i + 2) >= PIF_RAM_SIZE) {
                DebugMessage(M64MSG_WARNING, "Truncated PIF command ! Stopping PIF channel processing");
                i = PIF_RAM_SIZE;
                continue;
            }


            i += setup_pif_channel(&pif->channels[k++], &pif->ram[i]);
        }
    }

    /* Zilmar-Spec plugin expect a call with control_id = -1 when RAM processing is done */
    if (input.controllerCommand) {
        input.controllerCommand(-1, NULL);
    }

#ifdef DEBUG_PIF
    DebugMessage(M64MSG_INFO, "PIF setup channel");
    print_pif(pif);
#endif
}

static void process_cic_challenge(struct pif* pif)
{
    char challenge[30], response[30];
    size_t i;

    /* format the 'challenge' message into 30 nibbles for X-Scale's CIC code */
    for (i = 0; i < 15; ++i)
    {
        challenge[i*2]   = (pif->ram[0x30+i] >> 4) & 0x0f;
        challenge[i*2+1] =  pif->ram[0x30+i]       & 0x0f;
    }

    /* calculate the proper response for the given challenge (X-Scale's algorithm) */
    n64_cic_nus_6105(challenge, response, CHL_LEN - 2);
    pif->ram[0x2e] = 0;
    pif->ram[0x2f] = 0;

    /* re-format the 'response' into a byte stream */
    for (i = 0; i < 15; ++i)
    {
        pif->ram[0x30+i] = (response[i*2] << 4) + response[i*2+1];
    }

#ifdef DEBUG_PIF
    DebugMessage(M64MSG_INFO, "PIF cic challenge");
    print_pif(pif);
#endif
}

void poweron_pif(struct pif* pif)
{
    memset(pif->ram, 0, PIF_RAM_SIZE);

    reset_pif(pif, 0); /* cold reset */
}

void read_pif_mem(void* opaque, uint32_t address, uint32_t* value)
{
    struct pif* pif = (struct pif*)opaque;
    uint32_t addr = pif_address(address);

    memcpy(value, pif->base + addr, sizeof(*value));
    if (addr >= PIF_ROM_SIZE)
        *value = tohl(*value);
}

void write_pif_mem(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct pif* pif = (struct pif*)opaque;
    uint32_t addr = pif_address(address);

    if (addr < PIF_ROM_SIZE)
    {
        DebugMessage(M64MSG_ERROR, "Invalid write to PIF ROM: %08" PRIX32, address);
        return;
    }

    masked_write((uint32_t*)(&pif->base[addr]), fromhl(value), fromhl(mask));

    pif->si->dma_dir = SI_DMA_WRITE;

    cp0_update_count(pif->r4300);
    pif->si->regs[SI_STATUS_REG] |= (SI_STATUS_DMA_BUSY | SI_STATUS_IO_BUSY);
    add_interrupt_event(&pif->r4300->cp0, SI_INT, pif->si->dma_duration);
}


void process_pif_ram(struct pif* pif)
{
    uint8_t flags = pif->ram[0x3f];
    uint8_t clrmask = 0x00;
    size_t k;

    if (flags == 0) {
#ifdef DEBUG_PIF
        DebugMessage(M64MSG_INFO, "PIF process pif ram status=0x00");
        print_pif(pif);
#endif
        return;
    }

    if (flags & 0x01)
    {
        /* setup channels then clear format flag */
        setup_channels_format(pif);
        clrmask |= 0x01;
    }

    if (flags & 0x02)
    {
        /* disable channel processing when doing CIC challenge */
        for (k = 0; k < PIF_CHANNELS_COUNT; ++k) {
            disable_pif_channel(&pif->channels[k]);
        }

        /* CIC Challenge */
        process_cic_challenge(pif);
        clrmask |= 0x02;
    }

    if (flags & 0x08)
    {
        clrmask |= 0x08;
    }

    if (flags & 0x30)
    {
        pif->ram[0x3f] = 0x80;
    }

#ifdef DEBUG_PIF
    if (flags & 0xf4)
    {
        DebugMessage(M64MSG_ERROR, "error in process_pif_ram(): %" PRIX8, flags);
    }
#endif

    pif->ram[0x3f] &= ~clrmask;
}

void update_pif_ram(struct pif* pif)
{
    size_t k;
    int skipped_raw_pif_channel = 0;

    /* perform PIF/Channel communications */
    for (k = 0; k < PIF_CHANNELS_COUNT; ++k) {
        if (rollback_should_skip_raw_pif_channel(k)) {
            skipped_raw_pif_channel = 1;
            rollback_log_pif_channel("skip-before", k, &pif->channels[k]);
            rollback_force_controller_present(&pif->channels[k]);
            rollback_log_pif_channel("skip-after", k, &pif->channels[k]);
            continue;
        }
        rollback_log_pif_channel("native-before", k, &pif->channels[k]);
        process_channel(&pif->channels[k]);
        rollback_log_pif_channel("native-after", k, &pif->channels[k]);
    }

    /* Zilmar-Spec plugin expect a call with control_id = -1 when RAM processing is done */
    if (input.readController && !skipped_raw_pif_channel) {
        input.readController(-1, NULL);
    }

    netplay_update_input(pif);
    rollback_sync_input(pif);
    for (k = 0; k < PIF_CHANNELS_COUNT; ++k) {
        rollback_log_pif_channel("post-rollback", k, &pif->channels[k]);
    }
    call_pif_sync_callback(pif);

#ifdef DEBUG_PIF
    DebugMessage(M64MSG_INFO, "PIF post read");
    print_pif(pif);
#endif
}

void hw2_int_handler(void* opaque)
{
    struct pif* pif = (struct pif*)opaque;

    raise_maskable_interrupt(pif->r4300, CP0_CAUSE_IP4);
}
