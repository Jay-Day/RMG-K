/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - main.c                                                  *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2008-2009 Richard Goedeken                              *
 *   Copyright (C) 2008 Ebenblues Nmn Okaygo Tillin9                       *
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

/* This is MUPEN64's main entry point. It contains code that is common
 * to both the gui and non-gui versions of mupen64. See
 * gui subdirectories for the gui-specific code.
 * if you want to implement an interface, you should look here
 */

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/config.h"
#include "api/debugger.h"
#include "api/m64p_config.h"
#include "api/m64p_types.h"
#include "api/m64p_vidext.h"
#include "api/vidext.h"
#include "backends/api/audio_out_backend.h"
#include "backends/api/clock_backend.h"
#include "backends/api/controller_input_backend.h"
#include "backends/api/joybus.h"
#include "backends/api/rumble_backend.h"
#include "backends/api/storage_backend.h"
#include "backends/api/video_capture_backend.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "backends/clock_ctime_plus_delta.h"
#include "backends/file_storage.h"
#include "cheat.h"
#include "device/device.h"
#include "device/dd/disk.h"
#include "device/controllers/vru_controller.h"
#include "device/controllers/paks/biopak.h"
#include "device/controllers/paks/mempak.h"
#include "device/controllers/paks/rumblepak.h"
#include "device/controllers/paks/transferpak.h"
#include "device/gb/gb_cart.h"
#include "device/pif/bootrom_hle.h"
#include "device/r4300/interrupt.h"
#include "device/r4300/new_dynarec/new_dynarec.h"
#include "eventloop.h"
#include "main.h"
#include "osal/files.h"
#include "osal/preproc.h"
#include "osd/osd.h"
#include "plugin/plugin.h"
#if defined(PROFILE)
#include "profile.h"
#endif
#include "rom.h"
#include "savestates.h"
#include "screenshot.h"
#include "util.h"
#include "netplay.h"

#ifdef DBG
#include "debugger/dbg_debugger.h"
#endif

#ifdef WITH_LIRC
#include "lirc.h"
#endif //WITH_LIRC

/* version number for Core config section */
#define CONFIG_PARAM_VERSION 1.01

/** globals **/
m64p_handle g_CoreConfig = NULL;

m64p_frame_callback g_FrameCallback = NULL;

int         g_RomWordsLittleEndian = 0; // after loading, ROM words are in native N64 byte order (big endian). We will swap them on x86
int         g_EmulatorRunning = 0;      // need separate boolean to tell if emulator is running, since --nogui doesn't use a thread


int g_rom_pause;

struct cheat_ctx g_cheat_ctx;

/* g_mem_base is global to allow plugins early access (before device is initialized).
 * Do not use this variable directly in emulation code.
 * Initialization and DeInitialization of this variable is done at CoreStartup and CoreShutdown.
 */
void* g_mem_base = NULL;

uint32_t g_start_address = UINT32_C(0xa4000040);

struct device g_dev;

m64p_media_loader g_media_loader;

int g_gs_vi_counter = 0;

/** static (local) variables **/
static int   l_CurrentFrame = 0;         // frame counter
static int   l_TakeScreenshot = 0;       // Tell OSD Rendering callback to take a screenshot just before drawing the OSD
static int   l_SpeedFactor = 100;        // percentage of nominal game speed at which emulator is running
static int   l_FrameAdvance = 0;         // variable to check if we pause on next frame
static int   l_MainSpeedLimit = 1;       // insert delay during vi_interrupt to keep speed at real-time
static int   l_FrameOutputVideo = 1;      // allow video plugin screen updates
static int   l_FrameOutputAudio = 1;      // allow audio samples to be pushed
static int   l_FrameOutputPacing = 1;     // allow VI speed limiting and pause loop
static int   l_FrameOutputInput = 1;      // allow frontend hotkey/input polling during VI
static int   l_FrameRunActive = 0;        // restore output flags when frame run finishes
static int   l_FrameRunVideo = 1;
static int   l_FrameRunAudio = 1;
static int   l_FrameRunPacing = 1;
static int   l_FrameRunInput = 1;
static double l_RollbackTimesyncScale = 1.0;

/* -------------------------------------------------------------------------
 * Buffered core speed-limiter trace.
 *
 * Enabled by the frontend's Rollback -> Logging -> Pacing Trace setting.
 * Rows remain in memory during gameplay and are written after run_device()
 * returns, avoiding per-frame disk I/O.
 * ------------------------------------------------------------------------- */
#define RMGK_PACING_TRACE_CAPACITY 60000u

enum rmgk_pacing_reset_reason
{
    RMGK_PACING_RESET_NONE = 0,
    RMGK_PACING_RESET_INITIALIZE = 1,
    RMGK_PACING_RESET_TOO_LATE = 2,
    RMGK_PACING_RESET_TOO_EARLY = 3
};

struct rmgk_pacing_core_row
{
    uint64_t sequence;
    uint32_t core_frame;

    uint64_t entry_us;
    uint64_t entry_delta_us;
    uint64_t limiter_total_us;

    double expected_refresh_hz;
    int speed_factor;
    double speed_factor_multiple;
    double rollback_scale;
    double adjusted_limit_ms;
    int speed_limit_enabled;

    int rollback_execute_active;
    int visible_step_active;
    int hidden_step_active;
    int frame_output_pacing;

    int reset_before;
    int reset_after;
    int reset_reason;
    int presentation_pacer_bypass;

    double total_elapsed_game_ms;
    double elapsed_real_ms;
    double sleep_before_ms;
    double first_delay_request_ms;
    double requested_delay_total_ms;
    uint32_t delay_calls;
    uint32_t zero_delay_calls;
    uint64_t actual_delay_us;
    uint64_t max_single_delay_us;
    double sleep_after_ms;
};

static struct rmgk_pacing_core_row* l_RmgkPacingRows = NULL;
static size_t l_RmgkPacingRowCount = 0;
static uint64_t l_RmgkPacingLastEntryUs = 0;
static int l_RmgkPacingEnabled = 0;

static int l_RmgkPresentBaseHzPublished = 0;

static int rmgk_rollback_present_pacer_enabled(void)
{
    return 1;
}

static void rmgk_publish_present_base_hz(
    double expected_refresh_hz,
    double speed_factor_multiple)
{
    char value[64];
    double effective_hz;

    if (l_RmgkPresentBaseHzPublished)
        return;

    if (expected_refresh_hz <= 0.0 ||
        speed_factor_multiple <= 0.0)
    {
        return;
    }

    /*
     * The frontend applies rollback_scale itself, so publish only the
     * nominal VI rate after the ordinary core speed-factor adjustment.
     */
    effective_hz =
        expected_refresh_hz /
        speed_factor_multiple;

    snprintf(
        value,
        sizeof(value),
        "%.12f",
        effective_hz);

#if defined(_WIN32)
    _putenv_s(
        "RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE",
        value);
#else
    setenv(
        "RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE",
        value,
        1);
#endif

    l_RmgkPresentBaseHzPublished = 1;
}

static uint64_t rmgk_pacing_now_us(void)
{
    const uint64_t counter = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();

    if (frequency == 0)
        return 0;

    return (counter / frequency) * 1000000ULL +
           ((counter % frequency) * 1000000ULL) / frequency;
}

static void rmgk_pacing_trace_flush(void)
{
    char path[4096];
    const char* directory;
    const char* prefix;
    FILE* file;
    size_t i;

    if (!l_RmgkPacingEnabled || l_RmgkPacingRows == NULL)
        return;

    directory = getenv("RMGK_ROLLBACK_LOG_DIR");
    prefix = getenv("RMGK_ROLLBACK_LOG_PREFIX");

    if (directory != NULL && directory[0] != '\0' &&
        prefix != NULL && prefix[0] != '\0')
    {
        snprintf(
            path,
            sizeof(path),
            "%s/%s_pacing_core.csv",
            directory,
            prefix);
    }
    else
    {
        snprintf(
            path,
            sizeof(path),
            "rmgk_pacing_core.csv");
    }

    file = fopen(path, "wb");
    if (file != NULL)
    {
        fprintf(
            file,
            "sequence,core_frame,entry_us,entry_delta_us,"
            "limiter_total_us,expected_refresh_hz,speed_factor,"
            "speed_factor_multiple,rollback_scale,adjusted_limit_ms,"
            "speed_limit_enabled,rollback_execute_active,"
            "visible_step_active,hidden_step_active,frame_output_pacing,"
            "reset_before,reset_after,reset_reason,"
            "presentation_pacer_bypass,"
            "total_elapsed_game_ms,elapsed_real_ms,sleep_before_ms,"
            "first_delay_request_ms,requested_delay_total_ms,"
            "delay_calls,zero_delay_calls,actual_delay_us,"
            "max_single_delay_us,sleep_after_ms\n");

        for (i = 0; i < l_RmgkPacingRowCount; ++i)
        {
            const struct rmgk_pacing_core_row* row =
                &l_RmgkPacingRows[i];

            fprintf(
                file,
                "%llu,%u,%llu,%llu,%llu,"
                "%.9f,%d,%.9f,%.9f,%.9f,"
                "%d,%d,%d,%d,%d,"
                "%d,%d,%d,%d,"
                "%.9f,%.9f,%.9f,"
                "%.9f,%.9f,%u,%u,%llu,%llu,%.9f\n",
                (unsigned long long) row->sequence,
                row->core_frame,
                (unsigned long long) row->entry_us,
                (unsigned long long) row->entry_delta_us,
                (unsigned long long) row->limiter_total_us,
                row->expected_refresh_hz,
                row->speed_factor,
                row->speed_factor_multiple,
                row->rollback_scale,
                row->adjusted_limit_ms,
                row->speed_limit_enabled,
                row->rollback_execute_active,
                row->visible_step_active,
                row->hidden_step_active,
                row->frame_output_pacing,
                row->reset_before,
                row->reset_after,
                row->reset_reason,
                row->presentation_pacer_bypass,
                row->total_elapsed_game_ms,
                row->elapsed_real_ms,
                row->sleep_before_ms,
                row->first_delay_request_ms,
                row->requested_delay_total_ms,
                row->delay_calls,
                row->zero_delay_calls,
                (unsigned long long) row->actual_delay_us,
                (unsigned long long) row->max_single_delay_us,
                row->sleep_after_ms);
        }

        fclose(file);
    }

    free(l_RmgkPacingRows);
    l_RmgkPacingRows = NULL;
    l_RmgkPacingRowCount = 0;
    l_RmgkPacingLastEntryUs = 0;
    l_RmgkPacingEnabled = 0;
}

static void rmgk_pacing_trace_reset(int enabled)
{
    rmgk_pacing_trace_flush();

    l_RmgkPacingEnabled = enabled ? 1 : 0;

    if (!l_RmgkPacingEnabled)
        return;

    l_RmgkPacingRows =
        (struct rmgk_pacing_core_row*) calloc(
            RMGK_PACING_TRACE_CAPACITY,
            sizeof(struct rmgk_pacing_core_row));

    if (l_RmgkPacingRows == NULL)
        l_RmgkPacingEnabled = 0;
}

static void rmgk_pacing_trace_push(
    const struct rmgk_pacing_core_row* row)
{
    if (!l_RmgkPacingEnabled ||
        l_RmgkPacingRows == NULL ||
        row == NULL)
    {
        return;
    }

    if (l_RmgkPacingRowCount >= RMGK_PACING_TRACE_CAPACITY)
        return;

    l_RmgkPacingRows[l_RmgkPacingRowCount++] = *row;
}
static m64p_rollback_execute_callbacks l_RollbackExecuteCallbacks;
static int   l_RollbackExecuteActive = 0;
static int   l_RollbackSingleStepActive = 0;
static int   l_RollbackVisibleStepActive = 0;
static int   l_RollbackVisibleStepCompleted = 0;
/* Set only when the video plugin reached its pre-present rendering callback
 * for the current visible rollback frame. If it does not, the ordinary core
 * limiter remains active as a safety fallback. */
static int   l_RollbackPresentPacedThisFrame = 0;
static int   l_RollbackHiddenStepActive = 0;
static int   l_RollbackHiddenStepCompleted = 0;
static m64p_rollback_run_frame_stats l_RollbackRunFrameStats;
static uint32_t l_RollbackLoadProbePc = 0;
static uint32_t l_RollbackLoadProbeCp0Count = 0;
static uint32_t l_RollbackLoadProbeNextInterrupt = 0;
static uint32_t l_RollbackLoadProbeCurrentFrame = 0;
static int32_t l_RollbackLoadProbeCycleCount = 0;
static int32_t l_RollbackLoadProbePendingException = 0;
static int32_t l_RollbackLoadProbeStop = 0;
static uint32_t l_RollbackLoadBeforePc = 0;
static uint32_t l_RollbackLoadBeforeCp0Count = 0;
static uint32_t l_RollbackLoadBeforeNextInterrupt = 0;
static uint32_t l_RollbackLoadBeforeCurrentFrame = 0;
static int32_t l_RollbackLoadBeforeCycleCount = 0;
static int32_t l_RollbackLoadBeforePendingException = 0;
static int32_t l_RollbackLoadBeforeStop = 0;
static uint32_t l_RollbackResumeProbePc = 0;
static uint32_t l_RollbackResumeProbeCp0Count = 0;
static uint32_t l_RollbackResumeProbeNextInterrupt = 0;
static uint32_t l_RollbackResumeProbeCurrentFrame = 0;
static int32_t l_RollbackResumeProbeCycleCount = 0;
static int32_t l_RollbackResumeProbePendingException = 0;
static int32_t l_RollbackResumeProbeStop = 0;

static osd_message_t *l_msgVol = NULL;
static osd_message_t *l_msgFF = NULL;
static osd_message_t *l_msgPause = NULL;

/* compatible paks */
enum { PAK_MAX_SIZE = 5 };
static size_t l_paks_idx[GAME_CONTROLLERS_COUNT];
static void* l_paks[GAME_CONTROLLERS_COUNT][PAK_MAX_SIZE];
static const struct pak_interface* l_ipaks[PAK_MAX_SIZE];
static size_t l_pak_type_idx[6];

/* PRNG state - used for Mempaks ID generation */
static struct xoshiro256pp_state l_mpk_idgen;

/*********************************************************************************************************
* static functions
*/

static const char *get_savepathdefault(const char *configpath)
{
    static char path[1024];

    if (!configpath || (strlen(configpath) == 0)) {
        snprintf(path, 1024, "%ssave%c", ConfigGetUserDataPath(), OSAL_DIR_SEPARATORS[0]);
        path[1023] = 0;
    } else {
        snprintf(path, 1024, "%s%c", configpath, OSAL_DIR_SEPARATORS[0]);
        path[1023] = 0;
    }

    /* create directory if it doesn't exist */
    osal_mkdirp(path, 0700);

    return path;
}

static char *get_save_filename(void)
{
    static char filename[256];

    int format = ConfigGetParamInt(g_CoreConfig, "SaveFilenameFormat");

    if (format == 0) {
        snprintf(filename, 256, "%s", ROM_PARAMS.headername);
    } else /* if (format == 1) */ {
        if (strstr(ROM_SETTINGS.goodname, "(unknown rom)") == NULL) {
            snprintf(filename, 256, "%.32s-%.8s", ROM_SETTINGS.goodname, ROM_SETTINGS.MD5);
        } else if (ROM_HEADER.Name[0] != 0) {
            snprintf(filename, 256, "%s-%.8s", ROM_PARAMS.headername, ROM_SETTINGS.MD5);
        } else {
            snprintf(filename, 256, "unknown-%.8s", ROM_SETTINGS.MD5);
        }
    }

    /* sanitize filename */
    string_replace_chars(filename, ":<>\"/\\|?*", '_');

    return filename;
}

static char *get_mempaks_path(void)
{
    char *path;
    size_t size = 0;

    /* check if old file path exists, if it does then use that */
    path = formatstr("%s%s.mpk", get_savesrampath(), ROM_SETTINGS.goodname);
    if (get_file_size(path, &size) == file_ok && size > 0)
    {
        return path;
    }
    free(path);

    /* else use new path */
    return formatstr("%s%s.mpk", get_savesrampath(), get_save_filename());
}

static char *get_eeprom_path(void)
{
    char *path;
    size_t size = 0;

    /* check if old file path exists, if it does then use that */
    path = formatstr("%s%s.eep", get_savesrampath(), ROM_SETTINGS.goodname);
    if (get_file_size(path, &size) == file_ok && size > 0)
    {
        return path;
    }
    free(path);

    /* else use new path */
    return formatstr("%s%s.eep", get_savesrampath(), get_save_filename());
}

static char *get_sram_path(void)
{
    char *path;
    size_t size = 0;

    /* check if old file path exists, if it does then use that */
    path = formatstr("%s%s.sra", get_savesrampath(), ROM_SETTINGS.goodname);
    if (get_file_size(path, &size) == file_ok && size > 0)
    {
        return path;
    }
    free(path);

    /* else use new path */
    return formatstr("%s%s.sra", get_savesrampath(), get_save_filename());
}

static char *get_flashram_path(void)
{
    char *path;
    size_t size = 0;

    /* check if old file path exists, if it does then use that */
    path = formatstr("%s%s.fla", get_savesrampath(), ROM_SETTINGS.goodname);
    if (get_file_size(path, &size) == file_ok && size > 0)
    {
        return path;
    }
    free(path);

    /* else use new path */
    return formatstr("%s%s.fla", get_savesrampath(), get_save_filename());
}

static char *get_gb_ram_path(const char* gbrom, unsigned int control_id)
{
    return formatstr("%s%s.%u.sav", get_savesrampath(), gbrom, control_id);
}

static char *get_dd_disk_save_path(const char* disk, int format)
{
    char* filename = NULL;

    int len = strlen(disk);
    int has_expected_ext = (len >= 4 && (strcmp(disk + len - 4, ".ndd") == 0 || strcmp(disk + len - 4, ".d64") == 0));

    switch (format) {
    case 0: /* *.ndr,*.d6r, full disk content */
        if (has_expected_ext) {
            /* file has .ndd / .d64, so adjust existing extension */
            filename = formatstr("%s%s", get_savesrampath(), disk);
            len = strlen(filename);
            filename[len-1] = 'r';
        }
        else {
            /* file doesn't have .ndd / .d64 extension, so fallback to .ndr */
            filename = formatstr("%s%s.ndr", get_savesrampath(), disk);
        }
        break;
    case 1: /* *.ram, only RAM part is persisted */
        if (has_expected_ext) {
            /* file has .ndd / .d64, so adjust existing extension */
            filename = formatstr("%s%s", get_savesrampath(), disk);
            len = strlen(filename);
            filename[len-3] = 'r';
            filename[len-2] = 'a';
            filename[len-1] = 'm';
        }
        else {
            /* file doesn't have .ndd / .d64 extension, so fallback to .ram */
            filename = formatstr("%s%s.ram", get_savesrampath(), disk);
        }
        break;
    default:
        DebugMessage(M64MSG_WARNING, "Unexpected DD save format: %d", format);
        break;
    }
    return filename;
}


static m64p_error init_video_capture_backend(const struct video_capture_backend_interface** ivcap, void** vcap, m64p_handle config, const char* key)
{
    m64p_error err;

    const char* name = ConfigGetParamString(config, key);
    if (name == NULL) {
        DebugMessage(M64MSG_WARNING, "Couldn't get %s value. Using NULL value instead.", key);
    }

    /* try to find desired backend (by name) */
    *ivcap = get_video_capture_backend(name);

    /* handle not found case */
    if (*ivcap == NULL) {
        /* default to dummy backend */
        *ivcap = get_video_capture_backend(NULL);

        DebugMessage(M64MSG_WARNING, "Could not find %s video_capture_backend_interface. Using %s instead.",
            name, (*ivcap)->name);
    }

    /* build section name */
    char* section = formatstr("%s:%s", key, (*ivcap)->name);

    /* init backend */
    err = (*ivcap)->init(vcap, section);

    if (err == M64ERR_SUCCESS) {
        DebugMessage(M64MSG_INFO, "Using video capture backend: %s", (*ivcap)->name);
    }
    else {
        DebugMessage(M64MSG_ERROR, "Failed to initialize video capture backend %s: %s, falling back to dummy backend", (*ivcap)->name, CoreErrorMessage(err));
        /* fallback to dummy backend */
        *ivcap = get_video_capture_backend(NULL);
        (*ivcap)->init(vcap, section);
    }

    free(section);

    return err;
}

/*********************************************************************************************************
* helper functions
*/


const char *get_savestatepath(void)
{
    /* try to get the SaveStatePath string variable in the Core configuration section */
    return get_savepathdefault(ConfigGetParamString(g_CoreConfig, "SaveStatePath"));
}

const char *get_savesrampath(void)
{
    /* try to get the SaveSRAMPath string variable in the Core configuration section */
    return get_savepathdefault(ConfigGetParamString(g_CoreConfig, "SaveSRAMPath"));
}

const char *get_savestatefilename(void)
{
    /* return same file name as save files */
    return get_save_filename();
}

void main_message(m64p_msg_level level, unsigned int corner, const char *format, ...)
{
    va_list ap;
    char buffer[2049];
    va_start(ap, format);
    vsnprintf(buffer, 2047, format, ap);
    buffer[2048]='\0';
    va_end(ap);

    /* send message to on-screen-display if enabled */
    if (ConfigGetParamBool(g_CoreConfig, "OnScreenDisplay"))
        osd_new_message((enum osd_corner) corner, "%s", buffer);
    /* send message to front-end */
    DebugMessage(level, "%s", buffer);
}

static void main_check_inputs(void)
{
#ifdef WITH_LIRC
    lircCheckInput();
#endif
    SDL_PumpEvents();
}

/*********************************************************************************************************
* global functions, for adjusting the core emulator behavior
*/

int main_set_core_defaults(void)
{
    float fConfigParamsVersion;
    int bUpgrade = 0;

    if (ConfigGetParameter(g_CoreConfig, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "No version number in 'Core' config section. Setting defaults.");
        ConfigDeleteSection("Core");
        ConfigOpenSection("Core", &g_CoreConfig);
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'Core' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        ConfigDeleteSection("Core");
        ConfigOpenSection("Core", &g_CoreConfig);
    }
    else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
    {
        float fVersion = (float) CONFIG_PARAM_VERSION;
        ConfigSetParameter(g_CoreConfig, "Version", M64TYPE_FLOAT, &fVersion);
        DebugMessage(M64MSG_INFO, "Updating parameter set version in 'Core' config section to %.2f", fVersion);
        bUpgrade = 1;
    }

    /* parameters controlling the operation of the core */
    ConfigSetDefaultFloat(g_CoreConfig, "Version", (float) CONFIG_PARAM_VERSION,  "Mupen64Plus Core config parameter set version number.  Please don't change this version number.");
    ConfigSetDefaultBool(g_CoreConfig, "OnScreenDisplay", 1, "Draw on-screen display if True, otherwise don't draw OSD");
#if defined(DYNAREC)
    ConfigSetDefaultInt(g_CoreConfig, "R4300Emulator", 2, "Use Pure Interpreter if 0, Cached Interpreter if 1, or Dynamic Recompiler if 2 or more");
#else
    ConfigSetDefaultInt(g_CoreConfig, "R4300Emulator", 1, "Use Pure Interpreter if 0, Cached Interpreter if 1, or Dynamic Recompiler if 2 or more");
#endif
    ConfigSetDefaultBool(g_CoreConfig, "NoCompiledJump", 0, "Disable compiled jump commands in dynamic recompiler (should be set to False) ");
    ConfigSetDefaultBool(g_CoreConfig, "DisableExtraMem", 0, "Disable 4MB expansion RAM pack. May be necessary for some games");
    ConfigSetDefaultInt(g_CoreConfig, "CountPerOp", 0, "Force number of cycles per emulated instruction");
    ConfigSetDefaultInt(g_CoreConfig, "CountPerOpDenomPot", 0, "Reduce number of cycles per update by power of two when set greater than 0 (overclock)");
    ConfigSetDefaultBool(g_CoreConfig, "AutoStateSlotIncrement", 0, "Increment the save state slot after each save operation");
    ConfigSetDefaultInt(g_CoreConfig, "CurrentStateSlot", 0, "Save state slot (0-9) to use when saving/loading the emulator state");
    ConfigSetDefaultBool(g_CoreConfig, "EnableDebugger", 0, "Activate the R4300 debugger when ROM execution begins, if core was built with Debugger support");
    ConfigSetDefaultString(g_CoreConfig, "ScreenshotPath", "", "Path to directory where screenshots are saved. If this is blank, the default value of ${UserDataPath}/screenshot will be used");
    ConfigSetDefaultString(g_CoreConfig, "SaveStatePath", "", "Path to directory where emulator save states (snapshots) are saved. If this is blank, the default value of ${UserDataPath}/save will be used");
    ConfigSetDefaultString(g_CoreConfig, "SaveSRAMPath", "", "Path to directory where SRAM/EEPROM data (in-game saves) are stored. If this is blank, the default value of ${UserDataPath}/save will be used");
    ConfigSetDefaultString(g_CoreConfig, "SharedDataPath", "", "Path to a directory to search when looking for shared data files");
    ConfigSetDefaultBool(g_CoreConfig, "RandomizeInterrupt", 1, "Randomize PI/SI Interrupt Timing");
    ConfigSetDefaultInt(g_CoreConfig, "SiDmaDuration", -1, "Duration of SI DMA (-1: use per game settings)");
    ConfigSetDefaultString(g_CoreConfig, "GbCameraVideoCaptureBackend1", DEFAULT_VIDEO_CAPTURE_BACKEND, "Gameboy Camera Video Capture backend");
    ConfigSetDefaultInt(g_CoreConfig, "SaveDiskFormat", 1, "Disk Save Format (0: Full Disk Copy (*.ndr/*.d6r), 1: RAM Area Only (*.ram))");
    ConfigSetDefaultInt(g_CoreConfig, "SaveFilenameFormat", 1, "Save (SRAM/State) Filename Format (0: ROM Header Name, 1: Automatic (including partial MD5 hash))");
    ConfigSetDefaultBool(g_CoreConfig, "DisableSaveFileLoading", 0, "Disable loading of save files (SRAM/EEPROM/FlashRAM) - useful for Kaillera netplay");

    /* handle upgrades */
    if (bUpgrade)
    {
        if (fConfigParamsVersion < 1.01f)
        {  // added separate SaveSRAMPath parameter in v1.01
            const char *pccSaveStatePath = ConfigGetParamString(g_CoreConfig, "SaveStatePath");
            if (pccSaveStatePath != NULL)
                ConfigSetParameter(g_CoreConfig, "SaveSRAMPath", M64TYPE_STRING, pccSaveStatePath);
        }
    }

    /* set config parameters for keyboard and joystick commands */
    return event_set_core_defaults();
}

void main_speeddown(int percent)
{
    if (netplay_is_init())
        return;

    if (l_SpeedFactor - percent > 10)  /* 10% minimum speed */
    {
        l_SpeedFactor -= percent;
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "%s %d%%", "Playback speed:", l_SpeedFactor);
        audio.setSpeedFactor(l_SpeedFactor);
        StateChanged(M64CORE_SPEED_FACTOR, l_SpeedFactor);
    }
}

void main_speedup(int percent)
{
    if (netplay_is_init())
        return;

    if (l_SpeedFactor + percent < 300) /* 300% maximum speed */
    {
        l_SpeedFactor += percent;
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "%s %d%%", "Playback speed:", l_SpeedFactor);
        audio.setSpeedFactor(l_SpeedFactor);
        StateChanged(M64CORE_SPEED_FACTOR, l_SpeedFactor);
    }
}

static void main_speedset(int percent)
{
    if (netplay_is_init())
        return;

    if (percent < 1 || percent > 2000)
    {
        DebugMessage(M64MSG_WARNING, "Invalid speed setting %i percent", percent);
        return;
    }
    // disable fast-forward if it's enabled
    main_set_fastforward(0);
    // set speed
    l_SpeedFactor = percent;
    main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "%s %d%%", "Playback speed:", l_SpeedFactor);
    audio.setSpeedFactor(l_SpeedFactor);
    StateChanged(M64CORE_SPEED_FACTOR, l_SpeedFactor);
}

void main_set_fastforward(int enable)
{
    if (netplay_is_init())
        return;

    static int ff_state = 0;
    static int SavedSpeedFactor = 100;

    if (enable && !ff_state)
    {
        ff_state = 1; /* activate fast-forward */
        SavedSpeedFactor = l_SpeedFactor;
        l_SpeedFactor = 250;
        audio.setSpeedFactor(l_SpeedFactor);
        StateChanged(M64CORE_SPEED_FACTOR, l_SpeedFactor);
        // set fast-forward indicator
        l_msgFF = osd_new_message(OSD_TOP_RIGHT, "Fast Forward");
        osd_message_set_static(l_msgFF);
        osd_message_set_user_managed(l_msgFF);
    }
    else if (!enable && ff_state)
    {
        ff_state = 0; /* de-activate fast-forward */
        l_SpeedFactor = SavedSpeedFactor;
        audio.setSpeedFactor(l_SpeedFactor);
        StateChanged(M64CORE_SPEED_FACTOR, l_SpeedFactor);
        // remove message
        osd_delete_message(l_msgFF);
        l_msgFF = NULL;
    }

}

static void main_set_speedlimiter(int enable)
{
    if (netplay_is_init() && !netplay_lag())
        return;

    l_MainSpeedLimit = enable ? 1 : 0;
}

void main_speedlimiter_toggle(void)
{
    if (netplay_is_init())
        return;

    l_MainSpeedLimit = !l_MainSpeedLimit;
    main_set_speedlimiter(l_MainSpeedLimit);

    if (l_MainSpeedLimit) /* fix naturally occuring audio desync */
    {
        main_toggle_pause();
        SDL_Delay(1000);
        main_toggle_pause();
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Speed limiter enabled");
    }

    else
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Speed limiter disabled");
}

static int main_is_paused(void)
{
    return (g_EmulatorRunning && g_rom_pause);
}

void main_toggle_pause(void)
{
    if (!g_EmulatorRunning)
        return;

    if (netplay_is_init())
        return;

    if (g_rom_pause)
    {
        DebugMessage(M64MSG_STATUS, "Emulation continued.");
        if(l_msgPause)
        {
            osd_delete_message(l_msgPause);
            l_msgPause = NULL;
        }
        StateChanged(M64CORE_EMU_STATE, M64EMU_RUNNING);
    }
    else
    {
        if(l_msgPause)
            osd_delete_message(l_msgPause);

        DebugMessage(M64MSG_STATUS, "Emulation paused.");
        l_msgPause = osd_new_message(OSD_MIDDLE_CENTER, "Paused");
        osd_message_set_static(l_msgPause);
        osd_message_set_user_managed(l_msgPause);
        StateChanged(M64CORE_EMU_STATE, M64EMU_PAUSED);
    }

    g_rom_pause = !g_rom_pause;
    l_FrameAdvance = 0;
}

void main_advance_one(void)
{
    main_advance_frames(1);
}

void main_advance_frames(int frames)
{
    main_run_frames(frames, M64FRAME_OUTPUT_ALL);
}

void main_set_frame_output(int video, int audio, int pacing, int frontend_input)
{
    l_FrameOutputVideo = video ? 1 : 0;
    l_FrameOutputAudio = audio ? 1 : 0;
    l_FrameOutputPacing = pacing ? 1 : 0;
    l_FrameOutputInput = frontend_input ? 1 : 0;
}

void main_set_rollback_timesync_scale(double scale)
{
    if (scale < 0.99)
        scale = 0.99;
    else if (scale > 1.01)
        scale = 1.01;
    l_RollbackTimesyncScale = scale;
}

static uint64_t rollback_profile_now_us(void)
{
    uint64_t counter = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    return (counter / frequency) * 1000000ULL + ((counter % frequency) * 1000000ULL) / frequency;
}

void main_run_frames(int frames, int output_flags)
{
    if (frames < 1)
        frames = 1;

    if (!l_FrameRunActive) {
        l_FrameRunVideo = l_FrameOutputVideo;
        l_FrameRunAudio = l_FrameOutputAudio;
        l_FrameRunPacing = l_FrameOutputPacing;
        l_FrameRunInput = l_FrameOutputInput;
        l_FrameRunActive = 1;
    }

    main_set_frame_output(
        (output_flags & M64FRAME_OUTPUT_VIDEO) != 0,
        (output_flags & M64FRAME_OUTPUT_AUDIO) != 0,
        (output_flags & M64FRAME_OUTPUT_PACING) != 0,
        (output_flags & M64FRAME_OUTPUT_INPUT) != 0);

    l_FrameAdvance = frames;
    g_rom_pause = 0;
    StateChanged(M64CORE_EMU_STATE, M64EMU_RUNNING);
}

void main_set_rollback_execute_callbacks(m64p_rollback_execute_callbacks* callbacks)
{
    if (callbacks == NULL) {
        memset(&l_RollbackExecuteCallbacks, 0, sizeof(l_RollbackExecuteCallbacks));
        l_RollbackExecuteActive = 0;
        l_RollbackVisibleStepActive = 0;
        l_RollbackVisibleStepCompleted = 0;
        l_RollbackPresentPacedThisFrame = 0;
        l_RollbackHiddenStepActive = 0;
        l_RollbackHiddenStepCompleted = 0;
        l_RmgkPresentBaseHzPublished = 0;
#ifdef NEW_DYNAREC
        new_dynarec_rollback_stats_reset();
#endif
        r4300_cached_code_rollback_stats_reset();
        return;
    }

    l_RollbackExecuteCallbacks = *callbacks;
    l_RollbackExecuteActive =
        (l_RollbackExecuteCallbacks.begin_frame != NULL &&
         l_RollbackExecuteCallbacks.end_frame != NULL);
#ifdef NEW_DYNAREC
    new_dynarec_rollback_stats_reset();
#endif
    r4300_cached_code_rollback_stats_reset();
}

int main_rollback_execute_active(void)
{
    return l_RollbackExecuteActive;
}

int main_rollback_execute_begin_frame(void)
{
    return l_RollbackExecuteCallbacks.begin_frame(l_RollbackExecuteCallbacks.user_data);
}

int main_rollback_execute_end_frame(void)
{
    return l_RollbackExecuteCallbacks.end_frame(l_RollbackExecuteCallbacks.user_data);
}

void main_rollback_visible_frame_begin(void)
{
    l_RollbackVisibleStepActive = 1;
    l_RollbackVisibleStepCompleted = 0;
    l_RollbackPresentPacedThisFrame = 0;
}

int main_rollback_visible_frame_completed(void)
{
    int completed = l_RollbackVisibleStepCompleted;
    l_RollbackVisibleStepActive = 0;
    l_RollbackVisibleStepCompleted = 0;
    return completed;
}

static void main_rollback_read_cpu_probe(uint32_t* pc, uint32_t* cp0_count, uint32_t* next_interrupt,
    uint32_t* current_frame, int32_t* cycle_count, int32_t* pending_exception, int32_t* stop)
{
    uint32_t* cp0_regs = r4300_cp0_regs(&g_dev.r4300.cp0);

    *pc = *r4300_pc(&g_dev.r4300);
    *cp0_count = cp0_regs[CP0_COUNT_REG];
    *next_interrupt = *r4300_cp0_next_interrupt(&g_dev.r4300.cp0);
    *current_frame = l_CurrentFrame;
#ifdef NEW_DYNAREC
    *cycle_count = g_dev.r4300.new_dynarec_hot_state.cycle_count;
    *pending_exception = g_dev.r4300.new_dynarec_hot_state.pending_exception;
    *stop = g_dev.r4300.new_dynarec_hot_state.stop;
#else
    *cycle_count = *r4300_cp0_cycle_count(&g_dev.r4300.cp0);
    *pending_exception = 0;
    *stop = *r4300_stop(&g_dev.r4300);
#endif
}

void main_rollback_capture_load_probe(void)
{
    main_rollback_read_cpu_probe(&l_RollbackLoadProbePc, &l_RollbackLoadProbeCp0Count,
        &l_RollbackLoadProbeNextInterrupt, &l_RollbackLoadProbeCurrentFrame,
        &l_RollbackLoadProbeCycleCount, &l_RollbackLoadProbePendingException,
        &l_RollbackLoadProbeStop);
}

void main_rollback_capture_load_before_probe(void)
{
    main_rollback_read_cpu_probe(&l_RollbackLoadBeforePc, &l_RollbackLoadBeforeCp0Count,
        &l_RollbackLoadBeforeNextInterrupt, &l_RollbackLoadBeforeCurrentFrame,
        &l_RollbackLoadBeforeCycleCount, &l_RollbackLoadBeforePendingException,
        &l_RollbackLoadBeforeStop);
}

void main_rollback_capture_resume_probe(void)
{
    main_rollback_read_cpu_probe(&l_RollbackResumeProbePc, &l_RollbackResumeProbeCp0Count,
        &l_RollbackResumeProbeNextInterrupt, &l_RollbackResumeProbeCurrentFrame,
        &l_RollbackResumeProbeCycleCount, &l_RollbackResumeProbePendingException,
        &l_RollbackResumeProbeStop);
}

static void main_rollback_hidden_frame_begin(void)
{
    l_RollbackHiddenStepActive = 1;
    l_RollbackHiddenStepCompleted = 0;
    main_rollback_read_cpu_probe(&l_RollbackRunFrameStats.hidden_begin_pc,
        &l_RollbackRunFrameStats.hidden_begin_cp0_count,
        &l_RollbackRunFrameStats.hidden_begin_next_interrupt,
        &l_RollbackRunFrameStats.hidden_begin_current_frame,
        &l_RollbackRunFrameStats.hidden_begin_cycle_count,
        &l_RollbackRunFrameStats.hidden_begin_pending_exception,
        &l_RollbackRunFrameStats.hidden_begin_stop);
}

static int main_rollback_hidden_frame_completed(void)
{
    int completed = l_RollbackHiddenStepCompleted;
    l_RollbackHiddenStepActive = 0;
    l_RollbackHiddenStepCompleted = 0;
    return completed;
}

int main_rollback_hidden_frame_active(void)
{
    return l_RollbackHiddenStepActive;
}

int main_rollback_run_frame(int output_flags)
{
    int old_video = l_FrameOutputVideo;
    int old_audio = l_FrameOutputAudio;
    int old_pacing = l_FrameOutputPacing;
    int old_input = l_FrameOutputInput;
    uint32_t* cp0_regs;
    uint64_t total_begin;
    uint64_t r4300_begin;
    int result;

    memset(&l_RollbackRunFrameStats, 0, sizeof(l_RollbackRunFrameStats));
    l_RollbackRunFrameStats.output_flags = output_flags;
    l_RollbackRunFrameStats.emumode = g_dev.r4300.emumode;
    l_RollbackRunFrameStats.load_before_pc = l_RollbackLoadBeforePc;
    l_RollbackRunFrameStats.load_before_cp0_count = l_RollbackLoadBeforeCp0Count;
    l_RollbackRunFrameStats.load_before_next_interrupt = l_RollbackLoadBeforeNextInterrupt;
    l_RollbackRunFrameStats.load_before_current_frame = l_RollbackLoadBeforeCurrentFrame;
    l_RollbackRunFrameStats.load_before_cycle_count = l_RollbackLoadBeforeCycleCount;
    l_RollbackRunFrameStats.load_before_pending_exception = l_RollbackLoadBeforePendingException;
    l_RollbackRunFrameStats.load_before_stop = l_RollbackLoadBeforeStop;
    l_RollbackRunFrameStats.load_probe_pc = l_RollbackLoadProbePc;
    l_RollbackRunFrameStats.load_probe_cp0_count = l_RollbackLoadProbeCp0Count;
    l_RollbackRunFrameStats.load_probe_next_interrupt = l_RollbackLoadProbeNextInterrupt;
    l_RollbackRunFrameStats.load_probe_current_frame = l_RollbackLoadProbeCurrentFrame;
    l_RollbackRunFrameStats.load_probe_cycle_count = l_RollbackLoadProbeCycleCount;
    l_RollbackRunFrameStats.load_probe_pending_exception = l_RollbackLoadProbePendingException;
    l_RollbackRunFrameStats.load_probe_stop = l_RollbackLoadProbeStop;
    cp0_regs = r4300_cp0_regs(&g_dev.r4300.cp0);
    l_RollbackRunFrameStats.cp0_count_before = cp0_regs[CP0_COUNT_REG];
    l_RollbackRunFrameStats.next_interrupt_before = *r4300_cp0_next_interrupt(&g_dev.r4300.cp0);
    l_RollbackRunFrameStats.pc_before = *r4300_pc(&g_dev.r4300);
    l_RollbackRunFrameStats.current_frame_before = l_CurrentFrame;
    l_RollbackRunFrameStats.cp0_last_addr_before = *r4300_cp0_last_addr(&g_dev.r4300.cp0);
    l_RollbackRunFrameStats.delay_slot_before = g_dev.r4300.delay_slot;
#ifdef NEW_DYNAREC
    l_RollbackRunFrameStats.dynarec_pcaddr_before = g_dev.r4300.new_dynarec_hot_state.pcaddr;
    l_RollbackRunFrameStats.dynarec_cycle_count_before = g_dev.r4300.new_dynarec_hot_state.cycle_count;
    l_RollbackRunFrameStats.dynarec_pending_exception_before = g_dev.r4300.new_dynarec_hot_state.pending_exception;
    l_RollbackRunFrameStats.dynarec_stop_before = g_dev.r4300.new_dynarec_hot_state.stop;
#endif
    total_begin = rollback_profile_now_us();

#ifdef NEW_DYNAREC
    new_dynarec_rollback_stats_reset();
#endif
    r4300_cached_code_rollback_stats_reset();
    interrupt_rollback_stats_reset();

    main_set_frame_output(
        (output_flags & M64FRAME_OUTPUT_VIDEO) != 0,
        (output_flags & M64FRAME_OUTPUT_AUDIO) != 0,
        (output_flags & M64FRAME_OUTPUT_PACING) != 0,
        (output_flags & M64FRAME_OUTPUT_INPUT) != 0);

    main_rollback_hidden_frame_begin();
    r4300_begin = rollback_profile_now_us();
    result = run_r4300_current(&g_dev.r4300);
    l_RollbackRunFrameStats.r4300_us = rollback_profile_now_us() - r4300_begin;
    interrupt_rollback_stats_fill(&l_RollbackRunFrameStats);
    if (!main_rollback_hidden_frame_completed())
        result = 0;
    l_RollbackRunFrameStats.cp0_count_after = cp0_regs[CP0_COUNT_REG];
    l_RollbackRunFrameStats.next_interrupt_after = *r4300_cp0_next_interrupt(&g_dev.r4300.cp0);
    l_RollbackRunFrameStats.pc_after = *r4300_pc(&g_dev.r4300);
    l_RollbackRunFrameStats.current_frame_after = l_CurrentFrame;
    l_RollbackRunFrameStats.cp0_last_addr_after = *r4300_cp0_last_addr(&g_dev.r4300.cp0);
    l_RollbackRunFrameStats.delay_slot_after = g_dev.r4300.delay_slot;
#ifdef NEW_DYNAREC
    l_RollbackRunFrameStats.dynarec_pcaddr_after = g_dev.r4300.new_dynarec_hot_state.pcaddr;
    l_RollbackRunFrameStats.dynarec_cycle_count_after = g_dev.r4300.new_dynarec_hot_state.cycle_count;
    l_RollbackRunFrameStats.dynarec_pending_exception_after = g_dev.r4300.new_dynarec_hot_state.pending_exception;
    l_RollbackRunFrameStats.dynarec_stop_after = g_dev.r4300.new_dynarec_hot_state.stop;
#endif
    l_RollbackRunFrameStats.resume_probe_pc = l_RollbackResumeProbePc;
    l_RollbackRunFrameStats.resume_probe_cp0_count = l_RollbackResumeProbeCp0Count;
    l_RollbackRunFrameStats.resume_probe_next_interrupt = l_RollbackResumeProbeNextInterrupt;
    l_RollbackRunFrameStats.resume_probe_current_frame = l_RollbackResumeProbeCurrentFrame;
    l_RollbackRunFrameStats.resume_probe_cycle_count = l_RollbackResumeProbeCycleCount;
    l_RollbackRunFrameStats.resume_probe_pending_exception = l_RollbackResumeProbePendingException;
    l_RollbackRunFrameStats.resume_probe_stop = l_RollbackResumeProbeStop;

    main_set_frame_output(old_video, old_audio, old_pacing, old_input);
#ifdef NEW_DYNAREC
    {
        struct new_dynarec_rollback_stats dynarec_stats;
        new_dynarec_rollback_stats_get(&dynarec_stats);
        l_RollbackRunFrameStats.dynarec_recompile_count = dynarec_stats.recompile_count;
        l_RollbackRunFrameStats.dynarec_recompile_us = dynarec_stats.recompile_us;
        l_RollbackRunFrameStats.dynarec_invalidate_us = dynarec_stats.invalidate_us;
        l_RollbackRunFrameStats.dynarec_full_invalidate_count = dynarec_stats.full_invalidate_count;
        l_RollbackRunFrameStats.dynarec_range_invalidate_count = dynarec_stats.range_invalidate_count;
        l_RollbackRunFrameStats.dynarec_block_invalidate_count = dynarec_stats.block_invalidate_count;
        l_RollbackRunFrameStats.dynarec_verify_dirty_count = dynarec_stats.verify_dirty_count;
        l_RollbackRunFrameStats.dynarec_verify_dirty_us = dynarec_stats.verify_dirty_us;
        l_RollbackRunFrameStats.dynarec_get_addr_count = dynarec_stats.get_addr_count;
        l_RollbackRunFrameStats.dynarec_get_addr_us = dynarec_stats.get_addr_us;
        l_RollbackRunFrameStats.dynarec_get_addr_ht_count = dynarec_stats.get_addr_ht_count;
        l_RollbackRunFrameStats.dynarec_get_addr_32_count = dynarec_stats.get_addr_32_count;
        l_RollbackRunFrameStats.dynarec_dynamic_linker_count = dynarec_stats.dynamic_linker_count;
        l_RollbackRunFrameStats.dynarec_dynamic_linker_us = dynarec_stats.dynamic_linker_us;
        l_RollbackRunFrameStats.dynarec_dynamic_linker_ds_count = dynarec_stats.dynamic_linker_ds_count;
        l_RollbackRunFrameStats.dynarec_dynamic_linker_ds_us = dynarec_stats.dynamic_linker_ds_us;
        new_dynarec_rollback_stats_reset();
    }
#endif
    r4300_cached_code_rollback_stats_get(
        &l_RollbackRunFrameStats.cached_code_full_invalidate_count,
        &l_RollbackRunFrameStats.cached_code_range_invalidate_count);
    r4300_cached_code_rollback_stats_reset();
    l_RollbackRunFrameStats.total_us = rollback_profile_now_us() - total_begin;
    return result != 0;
}

void main_get_rollback_run_frame_stats(m64p_rollback_run_frame_stats* stats)
{
    if (stats == NULL)
        return;

    *stats = l_RollbackRunFrameStats;
}

int main_frame_video_enabled(void)
{
    return l_FrameOutputVideo;
}

int main_frame_audio_enabled(void)
{
    return l_FrameOutputAudio;
}

int main_frame_pacing_enabled(void)
{
    return l_FrameOutputPacing;
}

int main_frame_frontend_input_enabled(void)
{
    return l_FrameOutputInput;
}

static void main_draw_volume_osd(void)
{
    char msgString[64];
    const char *volString;

    // this calls into the audio plugin
    volString = audio.volumeGetString();
    if (volString == NULL)
    {
        strcpy(msgString, "Volume Not Supported.");
    }
    else
    {
        sprintf(msgString, "%s: %s", "Volume", volString);
    }

    // create a new message or update an existing one
    if (l_msgVol != NULL)
        osd_update_message(l_msgVol, "%s", msgString);
    else {
        l_msgVol = osd_new_message(OSD_MIDDLE_CENTER, "%s", msgString);
        osd_message_set_user_managed(l_msgVol);
    }
}

/* this function could be called as a result of a keypress, joystick/button movement,
   LIRC command, or 'testshots' command-line option timer */
void main_take_next_screenshot(void)
{
    l_TakeScreenshot = l_CurrentFrame + 1;
}

void main_state_set_slot(int slot)
{
    if (slot < 0 || slot > 9)
    {
        DebugMessage(M64MSG_WARNING, "Invalid savestate slot '%i' in main_state_set_slot().  Using 0", slot);
        slot = 0;
    }

    savestates_select_slot(slot);
}

void main_state_inc_slot(void)
{
    savestates_inc_slot();
}

void main_state_load(const char *filename)
{
    if (netplay_is_init())
        return;

    if (filename == NULL) // Save to slot
        savestates_set_job(savestates_job_load, savestates_type_m64p, NULL);
    else
        savestates_set_job(savestates_job_load, savestates_type_unknown, filename);
}

void main_state_save(int format, const char *filename)
{
    if (netplay_is_init())
        return;

    if (filename != NULL && strcmp(filename, "MEMORY") == 0)
    {
        savestates_request_rollback_save();
        return;
    }

    if (filename == NULL) // Save to slot
        savestates_set_job(savestates_job_save, savestates_type_m64p, NULL);
    else // Save to file
        savestates_set_job(savestates_job_save, (savestates_type)format, filename);
}

m64p_error main_core_state_query(m64p_core_param param, int *rval)
{
    switch (param)
    {
        case M64CORE_EMU_STATE:
            if (!g_EmulatorRunning)
                *rval = M64EMU_STOPPED;
            else if (g_rom_pause)
                *rval = M64EMU_PAUSED;
            else
                *rval = M64EMU_RUNNING;
            break;
        case M64CORE_VIDEO_MODE:
            if (!VidExt_VideoRunning())
                *rval = M64VIDEO_NONE;
            else if (VidExt_InFullscreenMode())
                *rval = M64VIDEO_FULLSCREEN;
            else
                *rval = M64VIDEO_WINDOWED;
            break;
        case M64CORE_SAVESTATE_SLOT:
            *rval = savestates_get_slot();
            break;
        case M64CORE_SPEED_FACTOR:
            *rval = l_SpeedFactor;
            break;
        case M64CORE_SPEED_LIMITER:
            *rval = l_MainSpeedLimit;
            break;
        case M64CORE_VIDEO_SIZE:
        {
            int width, height;
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            main_get_screen_size(&width, &height);
            *rval = (width << 16) + height;
            break;
        }
        case M64CORE_AUDIO_VOLUME:
        {
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;    
            return main_volume_get_level(rval);
        }
        case M64CORE_AUDIO_MUTE:
            *rval = main_volume_get_muted();
            break;
        case M64CORE_INPUT_GAMESHARK:
            *rval = event_gameshark_active();
            break;
        // these are only used for callbacks; they cannot be queried or set
        case M64CORE_SCREENSHOT_CAPTURED:
        case M64CORE_STATE_LOADCOMPLETE:
        case M64CORE_STATE_SAVECOMPLETE:
            return M64ERR_INPUT_INVALID;
        default:
            return M64ERR_INPUT_INVALID;
    }

    return M64ERR_SUCCESS;
}

m64p_error main_core_state_set(m64p_core_param param, int val)
{
    switch (param)
    {
        case M64CORE_EMU_STATE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (val == M64EMU_STOPPED)
            {        
                /* this stop function is asynchronous.  The emulator may not terminate until later */
                main_stop();
                return M64ERR_SUCCESS;
            }
            else if (val == M64EMU_RUNNING)
            {
                if (main_is_paused())
                    main_toggle_pause();
                return M64ERR_SUCCESS;
            }
            else if (val == M64EMU_PAUSED)
            {    
                if (!main_is_paused())
                    main_toggle_pause();
                return M64ERR_SUCCESS;
            }
            return M64ERR_INPUT_INVALID;
        case M64CORE_VIDEO_MODE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (val == M64VIDEO_WINDOWED)
            {
                if (VidExt_InFullscreenMode())
                    gfx.changeWindow();
                return M64ERR_SUCCESS;
            }
            else if (val == M64VIDEO_FULLSCREEN)
            {
                if (!VidExt_InFullscreenMode())
                    gfx.changeWindow();
                return M64ERR_SUCCESS;
            }
            return M64ERR_INPUT_INVALID;
        case M64CORE_SAVESTATE_SLOT:
            if (val < 0 || val > 9)
                return M64ERR_INPUT_INVALID;
            savestates_select_slot(val);
            return M64ERR_SUCCESS;
        case M64CORE_SPEED_FACTOR:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            main_speedset(val);
            return M64ERR_SUCCESS;
        case M64CORE_SPEED_LIMITER:
            main_set_speedlimiter(val);
            return M64ERR_SUCCESS;
        case M64CORE_VIDEO_SIZE:
        {
            // the front-end app is telling us that the user has resized the video output frame, and so
            // we should try to update the video plugin accordingly.  First, check state
            int width, height;
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            width = (val >> 16) & 0xffff;
            height = val & 0xffff;
            // then call the video plugin.  if the video plugin supports resizing, it will resize its viewport and call
            // VidExt_ResizeWindow to update the window manager handling our opengl output window
            gfx.resizeVideoOutput(width, height);
            return M64ERR_SUCCESS;
        }
        case M64CORE_AUDIO_VOLUME:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (val < 0 || val > 100)
                return M64ERR_INPUT_INVALID;
            return main_volume_set_level(val);
        case M64CORE_AUDIO_MUTE:
            if ((main_volume_get_muted() && !val) || (!main_volume_get_muted() && val))
                return main_volume_mute();
            return M64ERR_SUCCESS;
        case M64CORE_INPUT_GAMESHARK:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            event_set_gameshark(val);
            return M64ERR_SUCCESS;
        // these are only used for callbacks; they cannot be queried or set
        case M64CORE_STATE_LOADCOMPLETE:
        case M64CORE_STATE_SAVECOMPLETE:
            return M64ERR_INPUT_INVALID;
        default:
            return M64ERR_INPUT_INVALID;
    }
}

m64p_error main_get_screen_size(int *width, int *height)
{
    gfx.readScreen(NULL, width, height, 0);
    return M64ERR_SUCCESS;
}

m64p_error main_read_screen(void *pixels, int bFront)
{
    int width_trash, height_trash;
    gfx.readScreen(pixels, &width_trash, &height_trash, bFront);
    return M64ERR_SUCCESS;
}

m64p_error main_volume_up(void)
{
    int level = 0;
    audio.volumeUp();
    main_draw_volume_osd();
    main_volume_get_level(&level);
    StateChanged(M64CORE_AUDIO_VOLUME, level);
    return M64ERR_SUCCESS;
}

m64p_error main_volume_down(void)
{
    int level = 0;
    audio.volumeDown();
    main_draw_volume_osd();
    main_volume_get_level(&level);
    StateChanged(M64CORE_AUDIO_VOLUME, level);
    return M64ERR_SUCCESS;
}

m64p_error main_volume_get_level(int *level)
{
    *level = audio.volumeGetLevel();
    return M64ERR_SUCCESS;
}

m64p_error main_volume_set_level(int level)
{
    audio.volumeSetLevel(level);
    main_draw_volume_osd();
    level = audio.volumeGetLevel();
    StateChanged(M64CORE_AUDIO_VOLUME, level);
    return M64ERR_SUCCESS;
}

m64p_error main_volume_mute(void)
{
    audio.volumeMute();
    main_draw_volume_osd();
    StateChanged(M64CORE_AUDIO_MUTE, main_volume_get_muted());
    return M64ERR_SUCCESS;
}

int main_volume_get_muted(void)
{
    return (audio.volumeGetLevel() == 0);
}

m64p_error main_reset(int do_hard_reset)
{
    if (do_hard_reset) {
        hard_reset_device(&g_dev);
    }
    else {
        soft_reset_device(&g_dev);
    }

    return M64ERR_SUCCESS;
}

/*********************************************************************************************************
* global functions, callbacks from the r4300 core or from other plugins
*/

static void video_plugin_render_callback(int bScreenRedrawn)
{
#ifdef M64P_OSD
    int bOSD = ConfigGetParamBool(g_CoreConfig, "OnScreenDisplay");
#endif /* M64P_OSD */

    // if the flag is set to take a screenshot, then grab it now
    if (l_TakeScreenshot != 0)
    {
        // if the OSD is enabled, and the screen has not been recently redrawn, then we cannot take a screenshot now because
        // it contains the OSD text.  Wait until the next redraw
#ifdef M64P_OSD
        if (!bOSD || bScreenRedrawn)
#endif /* M64P_OSD */
        {
            TakeScreenshot(l_TakeScreenshot - 1);  // current frame number +1 is in l_TakeScreenshot
            l_TakeScreenshot = 0; // reset flag
        }
    }

#ifdef M64P_OSD
    // if the OSD is enabled, then draw it now
    if (bOSD)
    {
        osd_render();
    }
#endif /* M64P_OSD */

    // if the input plugin specified a render callback, call it now
    if(input.renderCallback)
    {
        input.renderCallback();
    }

    /*
     * All bundled video plugins invoke this rendering callback immediately
     * before presenting: GLideN64/Angrylion before GL swap and Parallel before
     * wsi->end_frame(). Pace here so OpenGL and Vulkan share one phase-locked
     * presentation path. Guard against duplicate rendering callbacks in one VI.
     */
    if (l_RollbackExecuteActive &&
        l_RollbackVisibleStepActive &&
        l_FrameOutputPacing &&
        !l_RollbackPresentPacedThisFrame &&
        l_RollbackExecuteCallbacks.pace_before_present != NULL)
    {
        l_RollbackPresentPacedThisFrame = 1;
        l_RollbackExecuteCallbacks.pace_before_present(
            l_RollbackExecuteCallbacks.user_data);
    }
}

void new_frame(void)
{
    pif_begin_rollback_input_frame();

    if (l_RollbackSingleStepActive) {
        stop_device(&g_dev);
        return;
    }

    if (!l_RollbackHiddenStepActive && g_FrameCallback != NULL)
        (*g_FrameCallback)(l_CurrentFrame);

    /* advance the current frame */
    l_CurrentFrame++;

    if (l_RollbackHiddenStepActive) {
        l_RollbackHiddenStepCompleted = 1;
        stop_device(&g_dev);
        return;
    }

    if (l_RollbackVisibleStepActive) {
        l_RollbackVisibleStepCompleted = 1;
        stop_device(&g_dev);
        return;
    }

    if (l_FrameAdvance > 0) {
        l_FrameAdvance--;
        if (l_FrameAdvance == 0) {
            if (l_FrameRunActive) {
                main_set_frame_output(l_FrameRunVideo, l_FrameRunAudio, l_FrameRunPacing, l_FrameRunInput);
                l_FrameRunActive = 0;
            }
            g_rom_pause = 1;
            StateChanged(M64CORE_EMU_STATE, M64EMU_PAUSED);
        }
    }
}

static void apply_speed_limiter(void)
{
    static unsigned long totalVIs = 0;
    static int resetOnce = 0;
    static int lastSpeedFactor = 100;
    static double totalElapsedGameTime = 0.0;
    static uint64_t StartFPSTime = 0;
    static const double defaultSpeedFactor = 100.0;

    struct rmgk_pacing_core_row traceRow;
    uint64_t traceEntryUs = 0;
    uint64_t traceEntryDeltaUs = 0;
    int traceResetReason = RMGK_PACING_RESET_NONE;

    if (l_RmgkPacingEnabled)
    {
        memset(&traceRow, 0, sizeof(traceRow));
        traceEntryUs = rmgk_pacing_now_us();
        traceEntryDeltaUs =
            l_RmgkPacingLastEntryUs == 0
                ? 0
                : traceEntryUs - l_RmgkPacingLastEntryUs;

        traceRow.sequence =
            (uint64_t) l_RmgkPacingRowCount;
        traceRow.core_frame = (uint32_t) l_CurrentFrame;
        traceRow.entry_us = traceEntryUs;
        traceRow.entry_delta_us = traceEntryDeltaUs;
        traceRow.speed_factor = l_SpeedFactor;
        traceRow.rollback_scale = l_RollbackTimesyncScale;
        traceRow.speed_limit_enabled = l_MainSpeedLimit;
        traceRow.rollback_execute_active =
            l_RollbackExecuteActive;
        traceRow.visible_step_active =
            l_RollbackVisibleStepActive;
        traceRow.hidden_step_active =
            l_RollbackHiddenStepActive;
        traceRow.frame_output_pacing =
            l_FrameOutputPacing;
        traceRow.reset_before = resetOnce;
    }

    uint64_t CurrentFPSTime = SDL_GetTicks();

    // calculate frame duration based upon ROM setting (50/60hz) and mupen64plus speed adjustment
    const double VILimitMilliseconds = 1000.0 / g_dev.vi.expected_refresh_rate;
    const double SpeedFactorMultiple = defaultSpeedFactor/l_SpeedFactor;
    const double AdjustedLimit = (VILimitMilliseconds * SpeedFactorMultiple) / l_RollbackTimesyncScale;

    if (l_RmgkPacingEnabled)
    {
        traceRow.expected_refresh_hz =
            g_dev.vi.expected_refresh_rate;
        traceRow.speed_factor_multiple =
            SpeedFactorMultiple;
        traceRow.adjusted_limit_ms =
            AdjustedLimit;
    }

    /*
     * Rollback presentation pacing.
     *
     * The video plugin rendering callback runs immediately before the actual
     * OpenGL/Vulkan present. Only bypass the ordinary core limiter if that hook
     * really ran for this visible frame. Plugins which omit the hook therefore
     * fall back to the core limiter instead of running unbounded.
     *
     * Reset the cumulative limiter state on every bypassed visible rollback
     * frame so stale wall-clock debt cannot leak back in when rollback
     * execution ends.
     */
    if (rmgk_rollback_present_pacer_enabled() &&
        l_RollbackExecuteActive &&
        l_RollbackVisibleStepActive &&
        l_FrameOutputPacing &&
        l_RollbackPresentPacedThisFrame)
    {
        rmgk_publish_present_base_hz(
            g_dev.vi.expected_refresh_rate,
            SpeedFactorMultiple);

        StartFPSTime = 0;
        totalVIs = 0;
        totalElapsedGameTime = 0.0;
        resetOnce = 0;
        lastSpeedFactor = l_SpeedFactor;

        if (l_RmgkPacingEnabled)
        {
            traceRow.presentation_pacer_bypass = 1;
            traceRow.total_elapsed_game_ms = 0.0;
            traceRow.elapsed_real_ms = 0.0;
            traceRow.sleep_before_ms = 0.0;
            traceRow.sleep_after_ms = 0.0;
            traceRow.reset_after = resetOnce;
            traceRow.reset_reason =
                RMGK_PACING_RESET_NONE;
            traceRow.limiter_total_us =
                rmgk_pacing_now_us() - traceEntryUs;

            rmgk_pacing_trace_push(&traceRow);
            l_RmgkPacingLastEntryUs = traceEntryUs;
        }

        return;
    }

    //if this is the first time or we are resuming from pause
    if(StartFPSTime == 0 || !resetOnce || lastSpeedFactor != l_SpeedFactor)
    {
       StartFPSTime = CurrentFPSTime;
       totalVIs = 0;
       totalElapsedGameTime = 0.0;
       resetOnce = 1;
       traceResetReason = RMGK_PACING_RESET_INITIALIZE;
    }
    else
    {
        ++totalVIs;
        totalElapsedGameTime += AdjustedLimit;
    }

    lastSpeedFactor = l_SpeedFactor;

#if defined(PROFILE)
    timed_section_start(TIMED_SECTION_IDLE);
#endif

#ifdef DBG
    if(g_DebuggerActive) DebuggerCallback(DEBUG_UI_VI, 0);
#endif

    double elapsedRealTime = CurrentFPSTime - StartFPSTime;
    double sleepTime = totalElapsedGameTime - elapsedRealTime;

    if (l_RmgkPacingEnabled)
    {
        traceRow.total_elapsed_game_ms =
            totalElapsedGameTime;
        traceRow.elapsed_real_ms = elapsedRealTime;
        traceRow.sleep_before_ms = sleepTime;
    }

    //Reset if the sleep needed is an unreasonable value
    static const double minSleepNeeded = -50;
    static const double maxSleepNeeded = 50;
    if(sleepTime < minSleepNeeded || sleepTime > (maxSleepNeeded*SpeedFactorMultiple))
    {
       resetOnce = 0;
       traceResetReason =
           sleepTime < minSleepNeeded
               ? RMGK_PACING_RESET_TOO_LATE
               : RMGK_PACING_RESET_TOO_EARLY;
    }

    if (sleepTime < minSleepNeeded) {
        totalElapsedGameTime = elapsedRealTime + minSleepNeeded;
    }

    if(l_MainSpeedLimit && sleepTime > 0 && sleepTime < maxSleepNeeded*SpeedFactorMultiple)
    {
        while(sleepTime >= 0) {
            const unsigned int requestedDelayMs =
                (unsigned int) sleepTime;
            uint64_t delayBeginUs = 0;
            uint64_t actualDelayUs = 0;

            if (l_RmgkPacingEnabled)
            {
                if (traceRow.delay_calls == 0)
                    traceRow.first_delay_request_ms = sleepTime;

                traceRow.requested_delay_total_ms +=
                    (double) requestedDelayMs;
                traceRow.delay_calls++;
                if (requestedDelayMs == 0)
                    traceRow.zero_delay_calls++;

                delayBeginUs = rmgk_pacing_now_us();
            }

            SDL_Delay(requestedDelayMs);

            if (l_RmgkPacingEnabled)
            {
                actualDelayUs =
                    rmgk_pacing_now_us() - delayBeginUs;
                traceRow.actual_delay_us += actualDelayUs;
                if (actualDelayUs >
                    traceRow.max_single_delay_us)
                {
                    traceRow.max_single_delay_us =
                        actualDelayUs;
                }
            }

            CurrentFPSTime = SDL_GetTicks();
            elapsedRealTime = CurrentFPSTime - StartFPSTime;
            sleepTime = totalElapsedGameTime - elapsedRealTime;
        }
    }


#if defined(PROFILE)
    timed_section_end(TIMED_SECTION_IDLE);
#endif

    if (l_RmgkPacingEnabled)
    {
        traceRow.limiter_total_us =
            rmgk_pacing_now_us() - traceEntryUs;
        traceRow.reset_after = resetOnce;
        traceRow.reset_reason = traceResetReason;
        traceRow.total_elapsed_game_ms =
            totalElapsedGameTime;
        traceRow.elapsed_real_ms = elapsedRealTime;
        traceRow.sleep_after_ms = sleepTime;

        rmgk_pacing_trace_push(&traceRow);
        l_RmgkPacingLastEntryUs = traceEntryUs;
    }
}

/* TODO: make a GameShark module and move that there */
static void gs_apply_cheats(struct cheat_ctx* ctx)
{
    struct r4300_core* r4300 = &g_dev.r4300;

    if (g_gs_vi_counter < 60)
    {
        if (g_gs_vi_counter == 0)
            cheat_apply_cheats(ctx, r4300, ENTRY_BOOT);
        g_gs_vi_counter++;
    }
    else
    {
        cheat_apply_cheats(ctx, r4300, ENTRY_VI);
    }
}

static void pause_loop(void)
{
    if(g_rom_pause)
    {
        osd_render();  // draw Paused message in case gfx.updateScreen didn't do it
        VidExt_GL_SwapBuffers();
        while(g_rom_pause)
        {
            SDL_Delay(10);
            main_check_inputs();
        }
    }
}

/* called on vertical interrupt.
 * Allow the core to perform various things */
void new_vi(void)
{
    int frame_output_pacing = main_frame_pacing_enabled();
    int frame_output_input = main_frame_frontend_input_enabled();
    uint64_t rollback_vi_begin = 0;
    uint64_t rollback_step_begin = 0;
    int rollback_profile_active = l_RollbackSingleStepActive || l_RollbackHiddenStepActive;

    if (rollback_profile_active)
        rollback_vi_begin = rollback_profile_now_us();

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    new_frame();
    if (rollback_profile_active)
        l_RollbackRunFrameStats.new_frame_us += rollback_profile_now_us() - rollback_step_begin;

#if defined(PROFILE)
    timed_sections_refresh();
#endif

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    gs_apply_cheats(&g_cheat_ctx);
    if (rollback_profile_active)
        l_RollbackRunFrameStats.cheats_us += rollback_profile_now_us() - rollback_step_begin;

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    if (frame_output_pacing)
        apply_speed_limiter();
    if (rollback_profile_active)
        l_RollbackRunFrameStats.pacing_us += rollback_profile_now_us() - rollback_step_begin;

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    if (frame_output_input)
        main_check_inputs();
    if (rollback_profile_active)
        l_RollbackRunFrameStats.input_us += rollback_profile_now_us() - rollback_step_begin;

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    if (main_frame_pacing_enabled())
        pause_loop();
    if (rollback_profile_active)
        l_RollbackRunFrameStats.pause_us += rollback_profile_now_us() - rollback_step_begin;

    if (rollback_profile_active)
        rollback_step_begin = rollback_profile_now_us();
    netplay_check_sync(&g_dev.r4300.cp0);
    if (rollback_profile_active)
        l_RollbackRunFrameStats.netplay_us += rollback_profile_now_us() - rollback_step_begin;

    if (rollback_profile_active)
        l_RollbackRunFrameStats.vi_us += rollback_profile_now_us() - rollback_vi_begin;
}

static void main_switch_pak(int control_id)
{
    struct game_controller* cont = &g_dev.controllers[control_id];

    change_pak(cont, l_paks[control_id][l_paks_idx[control_id]], l_ipaks[l_paks_idx[control_id]]);

    if (cont->ipak != NULL) {
        DebugMessage(M64MSG_INFO, "Controller %u pak changed to %s", control_id, cont->ipak->name);
    }
    else {
        DebugMessage(M64MSG_INFO, "Removing pak from controller %u", control_id);
    }
}

void main_switch_next_pak(int control_id)
{
    if (l_ipaks[l_paks_idx[control_id]] == NULL ||
        ++l_paks_idx[control_id] >= PAK_MAX_SIZE) {
        l_paks_idx[control_id] = 0;
    }

    main_switch_pak(control_id);
}

void main_switch_plugin_pak(int control_id)
{
    //Don't switch to the selected pak if it's not available for the game
    if (l_ipaks[l_pak_type_idx[Controls[control_id].Plugin]] == NULL) {
        Controls[control_id].Plugin = PLUGIN_NONE;
    }

    l_paks_idx[control_id] = l_pak_type_idx[Controls[control_id].Plugin];

    main_switch_pak(control_id);
}

static void open_mpk_file(struct file_storage* fstorage)
{
    unsigned int i;
    int ret = open_file_storage(fstorage, GAME_CONTROLLERS_COUNT*MEMPAK_SIZE, get_mempaks_path());

    if (ret == (int)file_open_error) {
        /* if file doesn't exists provide default content */
        for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {

            /* Generate a random serial ID */
            uint32_t serial[6];
            size_t k;
            for (k = 0; k < 6; ++k) {
                serial[k] = xoshiro256pp_next(&l_mpk_idgen);
            }

            format_mempak(fstorage->data + i * MEMPAK_SIZE,
                serial,
                DEFAULT_MEMPAK_DEVICEID,
                DEFAULT_MEMPAK_BANKS,
                DEFAULT_MEMPAK_VERSION);
        }
    }
}

static void open_fla_file(struct file_storage* fstorage)
{
    int ret = open_file_storage(fstorage, FLASHRAM_SIZE, get_flashram_path());

    if (ret == (int)file_open_error) {
        /* if file doesn't exists provide default content */
        format_flashram(fstorage->data);
    }
}

static void open_sra_file(struct file_storage* fstorage)
{
    int ret = open_file_storage(fstorage, SRAM_SIZE, get_sram_path());

    if (ret == (int)file_open_error) {
        /* if file doesn't exists provide default content */
        format_sram(fstorage->data);
    }
}

static void open_eep_file(struct file_storage* fstorage)
{
    /* Note: EEP files are all EEPROM_MAX_SIZE bytes long,
     * whatever the real EEPROM size is.
     */
    enum { EEPROM_MAX_SIZE = 0x800 };

    int ret = open_file_storage(fstorage, EEPROM_MAX_SIZE, get_eeprom_path());

    if (ret == (int)file_open_error) {
        /* if file doesn't exists provide default content */
        format_eeprom(fstorage->data, EEPROM_MAX_SIZE);
    }

    /* Truncate to 4k bit if necessary */
    if (ROM_SETTINGS.savetype != SAVETYPE_EEPROM_16K) {
        fstorage->size = 0x200;
    }
}

static void load_dd_rom(uint8_t* rom, size_t* rom_size, uint8_t* disk_region)
{
    /* set the DD rom region */
    if (g_media_loader.set_dd_rom_region != NULL)
    {
        g_media_loader.set_dd_rom_region(g_media_loader.cb_data, *disk_region);
    }

    /* ask the core loader for DD disk filename */
    char* dd_ipl_rom_filename = (g_media_loader.get_dd_rom == NULL)
        ? NULL
        : g_media_loader.get_dd_rom(g_media_loader.cb_data);

    if ((dd_ipl_rom_filename == NULL) || (strlen(dd_ipl_rom_filename) == 0)) {
        goto no_dd;
    }

    struct file_storage dd_rom;
    memset(&dd_rom, 0, sizeof(dd_rom));

    if (open_rom_file_storage(&dd_rom, dd_ipl_rom_filename) != file_ok) {
        DebugMessage(M64MSG_ERROR, "Failed to load DD IPL ROM: %s. Disabling 64DD", dd_ipl_rom_filename);
        goto no_dd;
    }

    DebugMessage(M64MSG_INFO, "DD IPL ROM: %s", dd_ipl_rom_filename);

    /* load and swap DD IPL ROM */
    *rom_size = g_ifile_storage_ro.size(&dd_rom);
    memcpy(rom, g_ifile_storage_ro.data(&dd_rom), *rom_size);
    close_file_storage(&dd_rom);

    /* fetch 1st word to identify IPL ROM format */
    /* FIXME: use more robust ROM detection heuristic - do the same for regular ROMs */
    uint32_t pi_bsd_dom1_config = 0
        | ((uint32_t)rom[0] << 24)
        | ((uint32_t)rom[1] << 16)
        | ((uint32_t)rom[2] <<  8)
        | ((uint32_t)rom[3] <<  0);

    switch (pi_bsd_dom1_config)
    {
    case 0x80270740: /* Z64 - big endian */
        to_big_endian_buffer(rom, 4, *rom_size/4);
        break;

    case 0x40072780: /* N64 - little endian */
        to_little_endian_buffer(rom, 4, *rom_size/4);
        break;

    case 0x27804007: /* V64 - bi-endian */
        swap_buffer(rom, 2, *rom_size/2);
        break;

    default: /* unknown */
        DebugMessage(M64MSG_ERROR, "Invalid DD IPL ROM: Disabling 64DD.");
        *rom_size = 0;
        return;
    }

    return;

no_dd:
    free(dd_ipl_rom_filename);
    *rom_size = 0;
}

static int load_dd_disk(struct dd_disk* dd_disk, const struct storage_backend_interface** dd_idisk)
{
    /* ask the core loader for DD disk filename */
    char* dd_disk_filename = (g_media_loader.get_dd_disk == NULL)
        ? NULL
        : g_media_loader.get_dd_disk(g_media_loader.cb_data);

    /* handle the no disk case */
    if (dd_disk_filename == NULL || strlen(dd_disk_filename) == 0) {
        goto no_disk;
    }

    /* Get DD Disk size */
    size_t dd_size = 0;
    if (get_file_size(dd_disk_filename, &dd_size) != file_ok) {
        DebugMessage(M64MSG_ERROR, "Can't get DD disk file size");
        goto no_disk;
    }

    struct file_storage* fstorage = malloc(sizeof(struct file_storage));
    struct file_storage* fstorage_save = malloc(sizeof(struct file_storage));
    if (fstorage == NULL || fstorage_save == NULL) {
        DebugMessage(M64MSG_ERROR, "Failed to allocate DD file_storage");
        if (fstorage != NULL)      { free(fstorage);      fstorage = NULL; }
        if (fstorage_save != NULL) { free(fstorage_save); fstorage_save = NULL; }
        goto no_disk;
    }

    /* Determine disk save format */
    int save_format = ConfigGetParamInt(g_CoreConfig, "SaveDiskFormat");
    /* MAME disks only support full disk save */
    if (dd_size == MAME_FORMAT_DUMP_SIZE && save_format != 0) {
        DebugMessage(M64MSG_WARNING, "MAME disks only support full disk save format, switching to full disk format !");
        save_format = 0;
    }

    /* Determine save file name */
    char* save_filename = get_dd_disk_save_path(namefrompath(dd_disk_filename), save_format);
    if (save_filename == NULL) {
        DebugMessage(M64MSG_ERROR, "Failed to get DD save path, DD will be read-only.");
        save_format = -1;
    }

    /* Try loading *.{nd,d6}r file first (if SaveDiskFormat == 0) */
    if (save_format == 0)
    {
        if (open_rom_file_storage(fstorage, save_filename) != file_ok) {
            DebugMessage(M64MSG_WARNING, "Failed to load DD Disk save: %s.", save_filename);

            /* Try loading regular disk file */
            if (open_rom_file_storage(fstorage, dd_disk_filename) != file_ok) {
                DebugMessage(M64MSG_ERROR, "Failed to load DD Disk: %s.", dd_disk_filename);
                goto free_fstorage;
            }
        }
    }
    else
    {
        /* Try loading regular disk file */
        if (open_rom_file_storage(fstorage, dd_disk_filename) != file_ok) {
            DebugMessage(M64MSG_ERROR, "Failed to load DD Disk: %s.", dd_disk_filename);
            goto free_fstorage;
        }
    }

    /* Force fstorage to point to save_filename, to redirect all writes to save file,
     * (and to avoid corrupting 64DD dump)
     * save_filename is now owned by fstorage.
     * dd_disk_filename is not owned anymore and must be freed individually.
     */
    fstorage->filename = save_filename;

    /* Scan disk to deduce disk format and other parameters and expand its size for D64 */
    unsigned int format = 0;
    unsigned int development = 0;
    size_t offset_sys = 0;
    size_t offset_id = 0;
    size_t offset_ram = 0;
    size_t size_ram = 0;
    uint8_t* new_data = scan_and_expand_disk_format(fstorage->data, fstorage->size, &format, &development, &offset_sys, &offset_id, &offset_ram, &size_ram);
    if (new_data == NULL) {
        DebugMessage(M64MSG_ERROR, "Wrong disk format");
        goto wrong_disk_format;
    }
    else {
        fstorage->data = new_data;
    }

    /* Load RAM save data (if SaveDiskFormat == 1) */
    if (save_format == 1)
    {
        if (read_from_file(save_filename, &fstorage->data[offset_ram], size_ram) != file_ok)
        {
            DebugMessage(M64MSG_WARNING, "Failed to load DD Disk RAM area (*.ram): %s.", save_filename);
        }
    }

    switch(save_format)
    {
    case 0: /* Full disk */
        *dd_idisk = &g_istorage_disk_full;
        fstorage_save->filename = save_filename;
        fstorage_save->data = fstorage->data;
        fstorage_save->size = fstorage->size;
        fstorage_save->first_access = 1;
        break;
    case 1: /* RAM only */
        *dd_idisk = &g_istorage_disk_ram_only;
        fstorage_save->filename = save_filename;
        fstorage_save->data = &fstorage->data[offset_ram];
        fstorage_save->size = size_ram;
        fstorage_save->first_access = 1;
        break;
    default: /* read only */
        *dd_idisk = &g_istorage_disk_read_only;
        free(fstorage_save);
        fstorage_save = NULL;
    }

    /* Setup dd_disk */
    dd_disk->storage = fstorage;
    dd_disk->istorage = &g_ifile_storage_ro;
    dd_disk->save_storage = fstorage_save;
    dd_disk->isave_storage = (save_format >= 0) ? &g_ifile_storage : NULL;
    dd_disk->format = format;
    dd_disk->development = development;
    dd_disk->region = DDREGION_UNKNOWN;
    dd_disk->offset_sys = offset_sys;
    dd_disk->offset_id = offset_id;
    dd_disk->offset_ram = offset_ram;

    /* Generate LBA conversion table */
    GenerateLBAToPhysTable(dd_disk);

    DebugMessage(M64MSG_INFO, "DD Disk: %s - %zu - %s",
            dd_disk_filename,
            (*dd_idisk)->size(dd_disk),
            get_disk_format_name(format));

    /* Get region from disk and byteswap it as needed */
    uint32_t w = *(uint32_t*)(*dd_idisk)->data(dd_disk);
    if (dd_disk->format == DISK_FORMAT_SDK) {
        swap_buffer(&w, sizeof(w), 1);
    }
    
    /* Set region in dd_disk */
    if (w == DD_REGION_DV || development) {
        dd_disk->region = DDREGION_DEV;
    } else if (w == DD_REGION_JP) {
        dd_disk->region = DDREGION_JAPAN;
    } else if (w == DD_REGION_US) {
        dd_disk->region = DDREGION_US;
    }

    if (w == DD_REGION_JP || w == DD_REGION_US || w == DD_REGION_DV) {
        DebugMessage(M64MSG_WARNING, "Loading a saved disk");
    }

    free(dd_disk_filename);
    return 1;

wrong_disk_format:
    /* no need to close save_storage as it is a child of disk->storage */
    close_file_storage(fstorage);
free_fstorage:
    free(fstorage);
    free(fstorage_save);
no_disk:
    free(dd_disk_filename);
    *dd_idisk = NULL;

    return 0;
}

static void close_dd_disk(struct dd_disk* disk)
{
    if (disk->save_storage != NULL) {
        /* no need to close save_storage as it is a child of disk->storage */
        free(disk->save_storage);
        disk->save_storage = NULL;
    }

    if (disk->storage != NULL) {
        close_file_storage(disk->storage);
        free(disk->storage);
        disk->storage = NULL;
    }
}


struct gb_cart_data
{
    int control_id;
    struct file_storage rom_fstorage;
    struct file_storage ram_fstorage;
    void* gbcam_backend;
    const struct video_capture_backend_interface* igbcam_backend;
};

static struct gb_cart_data l_gb_carts_data[GAME_CONTROLLERS_COUNT];

static void init_gb_rom(void* opaque, void** storage, const struct storage_backend_interface** istorage)
{
    struct gb_cart_data* data = (struct gb_cart_data*)opaque;

    /* Ask the core loader for rom filename */
    char* rom_filename = (g_media_loader.get_gb_cart_rom == NULL)
        ? NULL
        : g_media_loader.get_gb_cart_rom(g_media_loader.cb_data, data->control_id);

    /* Handle the no cart case */
    if (rom_filename == NULL || strlen(rom_filename) == 0) {
        goto no_cart;
    }

    /* Open ROM file */
    if (open_rom_file_storage(&data->rom_fstorage, rom_filename) != file_ok) {
        DebugMessage(M64MSG_ERROR, "Failed to load ROM file: %s", rom_filename);
        goto no_cart;
    }

    DebugMessage(M64MSG_INFO, "GB Loader ROM: %s - %zu",
            data->rom_fstorage.filename,
            data->rom_fstorage.size);

    /* init GB ROM storage */
    *storage = &data->rom_fstorage;
    *istorage = &g_ifile_storage_ro;
    return;

no_cart:
    free(rom_filename);
    *storage = NULL;
    *istorage = NULL;
}

static void release_gb_rom(void* opaque)
{
    struct gb_cart_data* data = (struct gb_cart_data*)opaque;

    close_file_storage(&data->rom_fstorage);

    memset(&data->rom_fstorage, 0, sizeof(data->rom_fstorage));
}

static void init_gb_ram(void* opaque, size_t ram_size, void** storage, const struct storage_backend_interface** istorage)
{
    struct gb_cart_data* data = (struct gb_cart_data*)opaque;

    /* Ask the core loader for ram filename */
    char* ram_filename = (g_media_loader.get_gb_cart_ram == NULL)
        ? NULL
        : g_media_loader.get_gb_cart_ram(g_media_loader.cb_data, data->control_id);

    /* Handle the no RAM case
     * if NULL or empty string generate a filename
     */
    if (ram_filename == NULL || strlen(ram_filename) == 0) {
        free(ram_filename);
        ram_filename = get_gb_ram_path(namefrompath(data->rom_fstorage.filename), data->control_id+1);
    }

    /* Open RAM file
     * if file doesn't exists provide default content */
    int err = open_file_storage(&data->ram_fstorage, ram_size, ram_filename);
    if (err == file_open_error) {
        memset(data->ram_fstorage.data, 0, data->ram_fstorage.size);
        DebugMessage(M64MSG_INFO, "Providing default RAM content");
    }
    else if (err == file_read_error) {
        DebugMessage(M64MSG_WARNING, "Size mismatch between expected RAM size and effective file size");
    }

    DebugMessage(M64MSG_INFO, "GB Loader RAM: %s - %zu",
            data->ram_fstorage.filename,
            data->ram_fstorage.size);

    /* init GB RAM storage */
    *storage = &data->ram_fstorage;
    *istorage = &g_ifile_storage;
}

static void release_gb_ram(void* opaque)
{
    struct gb_cart_data* data = (struct gb_cart_data*)opaque;

    close_file_storage(&data->ram_fstorage);

    memset(&data->ram_fstorage, 0, sizeof(data->ram_fstorage));
}

void main_change_gb_cart(int control_id)
{
    struct transferpak* tpk = &g_dev.transferpaks[control_id];
    struct gb_cart* gb_cart = &g_dev.gb_carts[control_id];
    struct gb_cart_data* data = &l_gb_carts_data[control_id];

    /* reset gb_cart_data */
    memset(data, 0, sizeof(*data));
    data->control_id = control_id;

    init_gb_cart(gb_cart,
            data, init_gb_rom, release_gb_rom,
            data, init_gb_ram, release_gb_ram,
            NULL, &g_iclock_ctime_plus_delta,
            &data->control_id, &g_irumble_backend_plugin_compat,
            data->gbcam_backend, data->igbcam_backend);

    if (gb_cart->read_gb_cart == NULL) {
        gb_cart = NULL;
    }

    change_gb_cart(tpk, gb_cart);

    if (tpk->gb_cart != NULL) {
        const uint8_t* rom_data = gb_cart->irom_storage->data(gb_cart->rom_storage);
        DebugMessage(M64MSG_INFO, "Inserting GB cart %s into transferpak %u", rom_data + 0x134, control_id);
    }
    else {
        DebugMessage(M64MSG_INFO, "Removing GB cart from transferpak %u", control_id);
    }
}


/*********************************************************************************************************
* emulation thread - runs the core
*/


m64p_error main_run(void)
{
    size_t i, k;
    size_t rdram_size;
    uint32_t count_per_op;
    uint32_t count_per_op_denom_pot;
    uint32_t emumode;
    uint32_t disable_extra_mem;
    int32_t si_dma_duration;
    int32_t no_compiled_jump;
    int32_t randomize_interrupt;
    struct file_storage eep;
    struct file_storage fla;
    struct file_storage sra;
    size_t dd_rom_size;
    struct dd_disk dd_disk;
    m64p_error failure_rval;

    int control_ids[GAME_CONTROLLERS_COUNT];
    struct controller_input_compat cin_compats[GAME_CONTROLLERS_COUNT];

    struct file_storage mpk_storages[GAME_CONTROLLERS_COUNT];
    struct file_storage mpk;

    void* gbcam_backend;
    const struct video_capture_backend_interface* igbcam_backend;

    /* XXX: select type of flashram from db */
    uint32_t flashram_type = MX29L1100_ID;

    uint16_t eeprom_type = JDT_NONE;
    switch (ROM_SETTINGS.savetype) {
        case SAVETYPE_EEPROM_4K:
            eeprom_type = JDT_EEPROM_4K;
            break;
        case SAVETYPE_EEPROM_16K:
            eeprom_type = JDT_EEPROM_16K;
            break;
    }

    /* Seed MPK ID gen using current time */
    uint64_t mpk_seed = !netplay_is_init() ? (uint64_t)time(NULL) : 0;
    l_mpk_idgen = xoshiro256pp_seed(mpk_seed);

    /* take the r4300 emulator mode from the config file at this point and cache it in a global variable */
    emumode = ConfigGetParamInt(g_CoreConfig, "R4300Emulator");

    /* set some other core parameters based on the config file values */
    savestates_set_autoinc_slot(ConfigGetParamBool(g_CoreConfig, "AutoStateSlotIncrement"));
    savestates_select_slot(ConfigGetParamInt(g_CoreConfig, "CurrentStateSlot"));
    no_compiled_jump = ConfigGetParamBool(g_CoreConfig, "NoCompiledJump");
    //We disable any randomness for netplay
    randomize_interrupt = !netplay_is_init() ? ConfigGetParamBool(g_CoreConfig, "RandomizeInterrupt") : 0;
    count_per_op = ConfigGetParamInt(g_CoreConfig, "CountPerOp");
    count_per_op_denom_pot = ConfigGetParamInt(g_CoreConfig, "CountPerOpDenomPot");

    if (ROM_SETTINGS.disableextramem)
        disable_extra_mem = ROM_SETTINGS.disableextramem;
    else
        disable_extra_mem = ConfigGetParamInt(g_CoreConfig, "DisableExtraMem");

    if (count_per_op <= 0)
        count_per_op = ROM_SETTINGS.countperop;

    if (count_per_op_denom_pot > 31)
        count_per_op_denom_pot = 31;

    si_dma_duration = ConfigGetParamInt(g_CoreConfig, "SiDmaDuration");
    if (si_dma_duration < 0)
        si_dma_duration = ROM_SETTINGS.sidmaduration;

    //During netplay, player 1 is the source of truth for these settings
    netplay_sync_settings(&count_per_op, &count_per_op_denom_pot, &disable_extra_mem, &si_dma_duration, &emumode, &no_compiled_jump);
    if (main_rollback_execute_active())
    {
        emumode = 2;
        no_compiled_jump = 0;
    }

    rdram_size = (disable_extra_mem == 0) ? 0x800000 : 0x400000;

    cheat_add_hacks(&g_cheat_ctx, ROM_PARAMS.cheats);

    /* do byte-swapping if it hasn't been done yet */
#if !defined(M64P_BIG_ENDIAN)
    if (g_RomWordsLittleEndian == 0)
    {
        swap_buffer((uint8_t*)mem_base_u32(g_mem_base, MM_CART_ROM), 4, g_rom_size/4);
        g_RomWordsLittleEndian = 1;
    }
#endif

    /* Fill-in l_pak_type_idx and l_ipaks according to game compatibility */
    k = 0;
    if (ROM_SETTINGS.biopak) {
        l_ipaks[k++] = &g_ibiopak;
    }
    if (ROM_SETTINGS.mempak) {
        l_pak_type_idx[PLUGIN_MEMPAK] = k;
        l_ipaks[k] = &g_imempak;
        ++k;
    }
    if (ROM_SETTINGS.rumble) {
        l_pak_type_idx[PLUGIN_RUMBLE_PAK] = k;
        l_pak_type_idx[PLUGIN_RAW] = k;
        l_ipaks[k] = &g_irumblepak;
        ++k;
    }
    if (ROM_SETTINGS.transferpak) {
        l_pak_type_idx[PLUGIN_TRANSFER_PAK] = k;
        l_ipaks[k] = &g_itransferpak;
        ++k;
    }
    l_pak_type_idx[PLUGIN_NONE] = k;
    l_ipaks[k] = NULL;

    if (!ROM_SETTINGS.mempak) {
        l_pak_type_idx[PLUGIN_MEMPAK] = k;
    }
    if (!ROM_SETTINGS.rumble) {
        l_pak_type_idx[PLUGIN_RUMBLE_PAK] = k;
        l_pak_type_idx[PLUGIN_RAW] = k;
    }
    if (!ROM_SETTINGS.transferpak) {
        l_pak_type_idx[PLUGIN_TRANSFER_PAK] = k;
    }

    /* init GbCamera backend specified in the configuration file */
    init_video_capture_backend(&igbcam_backend, &gbcam_backend,
        g_CoreConfig, "GbCameraVideoCaptureBackend1");    

    /* open storage files, provide default content if not present */
    open_mpk_file(&mpk);
    open_eep_file(&eep);
    open_fla_file(&fla);
    open_sra_file(&sra);

    /* Load 64DD IPL ROM and Disk */
    const struct clock_backend_interface* dd_rtc_iclock = NULL;
    const struct storage_backend_interface* dd_idisk = NULL;
    memset(&dd_disk, 0, sizeof(dd_disk));

    /* try to load DD disk first, if that succeeds, pass the region to load_dd_rom */
    if (load_dd_disk(&dd_disk, &dd_idisk))
    {
        dd_rtc_iclock = &g_iclock_ctime_plus_delta;
        load_dd_rom((uint8_t*)mem_base_u32(g_mem_base, MM_DD_ROM), &dd_rom_size, &dd_disk.region);
    }
    else
    {
        dd_rom_size = 0;
    }

    /* ensure the 64DD rom & disk are loaded,
     * otherwise we have to bail right now */
    if (g_rom_size == 0 && dd_rom_size == 0)
    {
        goto on_disk_failure;
    }

    /* setup pif channel devices */
    void* joybus_devices[PIF_CHANNELS_COUNT];
    const struct joybus_device_interface* ijoybus_devices[PIF_CHANNELS_COUNT];

    memset(&g_dev.gb_carts, 0, GAME_CONTROLLERS_COUNT*sizeof(*g_dev.gb_carts));
    memset(&l_gb_carts_data, 0, GAME_CONTROLLERS_COUNT*sizeof(*l_gb_carts_data));
    memset(cin_compats, 0, GAME_CONTROLLERS_COUNT*sizeof(*cin_compats));

    netplay_read_registration(cin_compats);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {

        //During netplay, we "trick" the input plugin
        //by replacing the regular control_id with the ID that is controlling the player during netplay
        control_ids[i] = netplay_is_init() ? netplay_get_controller(i) : (int)i;

        /* if input plugin requests RawData let the input plugin do the channel device processing */
        if (Controls[i].RawData) {
            joybus_devices[i] = &control_ids[i];
            ijoybus_devices[i] = &g_ijoybus_device_plugin_compat;
        }
        else if (Controls[i].Type == CONT_TYPE_VRU) {
            const struct game_controller_flavor* cont_flavor =
                &g_vru_controller_flavor;
            joybus_devices[i] = &g_dev.controllers[i];
            ijoybus_devices[i] = &g_ijoybus_vru_controller;

            cin_compats[i].control_id = (int)i;
            cin_compats[i].cont = &g_dev.controllers[i];
            cin_compats[i].last_pak_type = Controls[i].Plugin;
            cin_compats[i].last_input = 0;
            cin_compats[i].netplay_count = 0;
            cin_compats[i].event_first = NULL;

            Controls[i].Plugin = PLUGIN_NONE;

            /* init vru_controller */
            init_game_controller(&g_dev.controllers[i],
                    cont_flavor,
                    &cin_compats[i], &g_icontroller_input_backend_plugin_compat,
                    NULL, NULL);
        }
        /* otherwise let the core do the processing */
        else {
            /* select appropriate controller
             * FIXME: assume for now that only standard controller is compatible
             * Use the rom db to know if other peripherals are compatibles (VRU, mouse, train, ...)
             */
            const struct game_controller_flavor* cont_flavor =
                &g_standard_controller_flavor;

            joybus_devices[i] = &g_dev.controllers[i];
            ijoybus_devices[i] = &g_ijoybus_device_controller;

            cin_compats[i].control_id = (int)i;
            cin_compats[i].cont = &g_dev.controllers[i];
            cin_compats[i].tpk = &g_dev.transferpaks[i];
            cin_compats[i].last_pak_type = Controls[i].Plugin;
            cin_compats[i].last_input = 0;
            cin_compats[i].netplay_count = 0;
            cin_compats[i].event_first = NULL;

            l_gb_carts_data[i].control_id = (int)i;

            l_gb_carts_data[i].gbcam_backend = gbcam_backend;
            l_gb_carts_data[i].igbcam_backend = igbcam_backend;

            l_paks_idx[i] = 0;

            //Don't use the selected pak if it's not available for the game, instead use NONE
            if (l_ipaks[l_pak_type_idx[Controls[i].Plugin]] == NULL) {
                Controls[i].Plugin = PLUGIN_NONE;
            }

            /* init all compatibles paks */
            for(k = 0; k < PAK_MAX_SIZE; ++k) {
                /* Bio Pak */
                if (l_ipaks[k] == &g_ibiopak) {
                    init_biopak(&g_dev.biopaks[i], 64);
                    l_paks[i][k] = &g_dev.biopaks[i];

                    if (Controls[i].Plugin == PLUGIN_BIO_PAK) {
                        l_paks_idx[i] = k;
                    }
                }
                /* Memory Pak */
                else if (l_ipaks[k] == &g_imempak) {
                    mpk_storages[i].data = mpk.data + i * MEMPAK_SIZE;
                    mpk_storages[i].size = MEMPAK_SIZE;
                    mpk_storages[i].filename = (void*)&mpk; /* OK for isubfile_storage */

                    init_mempak(&g_dev.mempaks[i], &mpk_storages[i], &g_isubfile_storage);
                    l_paks[i][k] = &g_dev.mempaks[i];

                    if (Controls[i].Plugin == PLUGIN_MEMPAK) {
                        l_paks_idx[i] = k;
                    }
                }
                /* Rumble Pak */
                else if (l_ipaks[k] == &g_irumblepak) {
                    init_rumblepak(&g_dev.rumblepaks[i], &control_ids[i], &g_irumble_backend_plugin_compat);
                    l_paks[i][k] = &g_dev.rumblepaks[i];

                    if (Controls[i].Plugin == PLUGIN_RUMBLE_PAK
                     || Controls[i].Plugin == PLUGIN_RAW) {
                        l_paks_idx[i] = k;
                    }
                }
                /* Transfer Pak */
                else if (l_ipaks[k] == &g_itransferpak) {

                    /* init GB cart */
                    init_gb_cart(&g_dev.gb_carts[i],
                            &l_gb_carts_data[i], init_gb_rom, release_gb_rom,
                            &l_gb_carts_data[i], init_gb_ram, release_gb_ram,
                            NULL, &g_iclock_ctime_plus_delta,
                            &l_gb_carts_data[i].control_id, &g_irumble_backend_plugin_compat,
                            l_gb_carts_data[i].gbcam_backend, l_gb_carts_data[i].igbcam_backend);

                    init_transferpak(&g_dev.transferpaks[i], (g_dev.gb_carts[i].read_gb_cart == NULL) ? NULL : &g_dev.gb_carts[i]);
                    l_paks[i][k] = &g_dev.transferpaks[i];

                    if (Controls[i].Plugin == PLUGIN_TRANSFER_PAK) {
                        l_paks_idx[i] = k;
                    }

                    /* enable GB cart switch */
                    cin_compats[i].gb_cart_switch_enabled = 1;
                }
                /* No Pak */
                else {
                    l_ipaks[k] = NULL;
                    l_paks[i][k] = NULL;

                    if (Controls[i].Plugin == PLUGIN_NONE) {
                        l_paks_idx[i] = k;
                    }

                    break;
                }
            }

            /* init game_controller */
            init_game_controller(&g_dev.controllers[i],
                    cont_flavor,
                    &cin_compats[i], &g_icontroller_input_backend_plugin_compat,
                    l_paks[i][l_paks_idx[i]], l_ipaks[l_paks_idx[i]]);

            if (l_ipaks[l_paks_idx[i]] != NULL) {
                DebugMessage(M64MSG_INFO, "Game controller %u (%s) has a %s plugged in",
                    (uint32_t) i, cont_flavor->name, l_ipaks[l_paks_idx[i]]->name);
            } else {
                DebugMessage(M64MSG_INFO, "Game controller %u (%s) has nothing plugged in",
                    (uint32_t) i, cont_flavor->name);
            }
        }
    }
    for (i = GAME_CONTROLLERS_COUNT; i < PIF_CHANNELS_COUNT; ++i) {
        joybus_devices[i] = &g_dev.cart;
        ijoybus_devices[i] = &g_ijoybus_device_cart;
    }

    init_device(&g_dev,
                g_mem_base,
                emumode,
                count_per_op,
                count_per_op_denom_pot,
                no_compiled_jump,
                randomize_interrupt,
                g_start_address,
                &g_dev.ai, &g_iaudio_out_backend_plugin_compat, ((float)ROM_SETTINGS.aidmamodifier / 100.0),
                si_dma_duration,
                rdram_size,
                joybus_devices, ijoybus_devices,
                vi_clock_from_tv_standard(ROM_PARAMS.systemtype), vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype),
                NULL, &g_iclock_ctime_plus_delta,
                g_rom_size,
                eeprom_type,
                &eep, &g_ifile_storage,
                flashram_type,
                &fla, &g_ifile_storage,
                &sra, &g_ifile_storage,
                NULL, dd_rtc_iclock,
                dd_rom_size,
                &dd_disk, dd_idisk);

    // Attach rom to plugins
    failure_rval = M64ERR_PLUGIN_FAIL;
    if (!gfx.romOpen())
    {
        goto on_gfx_open_failure;
    }
    if (!audio.romOpen())
    {
        goto on_audio_open_failure;
    }
    if (!input.romOpen())
    {
        goto on_input_open_failure;
    }

    /* set up the SDL key repeat and event filter to catch keyboard/joystick commands for the core */
    event_initialize();

    /* initialize frame counter */
    l_CurrentFrame = 0;

    /* initialize the on-screen display */
    if (ConfigGetParamBool(g_CoreConfig, "OnScreenDisplay"))
    {
        // init on-screen display
        int width = 640, height = 480;
        gfx.readScreen(NULL, &width, &height, 0); // read screen to get width and height
        osd_init(width, height);
    }

    // setup rendering callback from video plugin to the core, for screenshots and On-Screen-Display
    gfx.setRenderingCallback(video_plugin_render_callback);

#ifdef WITH_LIRC
    lircStart();
#endif // WITH_LIRC

#ifdef DBG
    if (ConfigGetParamBool(g_CoreConfig, "EnableDebugger"))
        init_debugger();
#endif

    /* Startup message on the OSD */
    osd_new_message(OSD_MIDDLE_CENTER, "Mupen64Plus Started...");

    g_EmulatorRunning = 1;
    StateChanged(M64CORE_EMU_STATE, M64EMU_RUNNING);

    rmgk_pacing_trace_reset(
        l_RollbackExecuteCallbacks.pacing_trace_enabled);
    poweron_device(&g_dev);
    pif_bootrom_hle_execute(&g_dev.r4300);
    run_device(&g_dev);
    rmgk_pacing_trace_flush();

    /* now begin to shut down */
#ifdef WITH_LIRC
    lircStop();
#endif // WITH_LIRC

#ifdef DBG
    if (g_DebuggerActive)
        destroy_debugger();
#endif
    /* release gb_carts */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        if (!Controls[i].RawData  && (Controls[i].Type == CONT_TYPE_STANDARD) && g_dev.gb_carts[i].read_gb_cart != NULL) {
            release_gb_rom(&l_gb_carts_data[i]);
            release_gb_ram(&l_gb_carts_data[i]);
        }
    }

    igbcam_backend->close(gbcam_backend);
    igbcam_backend->release(gbcam_backend);

    close_file_storage(&sra);
    close_file_storage(&fla);
    close_file_storage(&eep);
    close_file_storage(&mpk);
    close_dd_disk(&dd_disk);

    /* reset pif */
    close_pif();

    if (ConfigGetParamBool(g_CoreConfig, "OnScreenDisplay"))
    {
        osd_exit();
    }

    rsp.romClosed();
    input.romClosed();
    audio.romClosed();
    gfx.romClosed();

    // clean up
    g_EmulatorRunning = 0;
    StateChanged(M64CORE_EMU_STATE, M64EMU_STOPPED);

    return M64ERR_SUCCESS;

on_disk_failure:
    failure_rval = M64ERR_INVALID_STATE;
    rsp.romClosed();
    input.romClosed();
on_input_open_failure:
    audio.romClosed();
on_audio_open_failure:
    gfx.romClosed();
on_gfx_open_failure:
    /* release gb_carts */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        if (!Controls[i].RawData  && (Controls[i].Type == CONT_TYPE_STANDARD) && g_dev.gb_carts[i].read_gb_cart != NULL) {
            release_gb_rom(&l_gb_carts_data[i]);
            release_gb_ram(&l_gb_carts_data[i]);
        }
    }

    igbcam_backend->close(gbcam_backend);
    igbcam_backend->release(gbcam_backend);

    /* release storage files */
    close_file_storage(&sra);
    close_file_storage(&fla);
    close_file_storage(&eep);
    close_file_storage(&mpk);
    close_dd_disk(&dd_disk);

    /* reset pif */
    close_pif();

    rmgk_pacing_trace_flush();
    return failure_rval;
}

void main_stop(void)
{
    /* note: this operation is asynchronous.  It may be called from a thread other than the
       main emulator thread, and may return before the emulator is completely stopped */
    if (!g_EmulatorRunning)
        return;

    DebugMessage(M64MSG_STATUS, "Stopping emulation.");
    main_set_frame_output(1, 1, 1, 1);
    l_FrameRunActive = 0;
    if(l_msgPause)
    {
        osd_delete_message(l_msgPause);
        l_msgPause = NULL;
    }
    if(l_msgFF)
    {
        osd_delete_message(l_msgFF);
        l_msgFF = NULL;
    }
    if(l_msgVol)
    {
        osd_delete_message(l_msgVol);
        l_msgVol = NULL;
    }
    if (g_rom_pause)
    {
        g_rom_pause = 0;
        StateChanged(M64CORE_EMU_STATE, M64EMU_RUNNING);
    }

    stop_device(&g_dev);

#ifdef DBG
    if(g_DebuggerActive)
    {
        debugger_step();
    }
#endif
}

m64p_error open_pif(const unsigned char* pifimage, unsigned int size)
{
    md5_byte_t old_pif_ntsc_md5[] = {0x49, 0x21, 0xD5, 0xF2, 0x16, 0x5D, 0xEE, 0x6E, 0x24, 0x96, 0xF4, 0x38, 0x8C, 0x4C, 0x81, 0xDA};
    md5_byte_t old_pif_pal_md5[]  = {0x2B, 0x6E, 0xEC, 0x58, 0x6F, 0xAA, 0x43, 0xF3, 0x46, 0x23, 0x33, 0xB8, 0x44, 0x83, 0x45, 0x54};

    md5_byte_t pif_ntsc_md5[] = {0x5C, 0x12, 0x4E, 0x79, 0x48, 0xAD, 0xA8, 0x5D, 0xA6, 0x03, 0xA5, 0x22, 0x78, 0x29, 0x40, 0xD0};
    md5_byte_t pif_pal_md5[]  = {0xD4, 0x23, 0x2D, 0xC9, 0x35, 0xCA, 0xD0, 0x65, 0x0A, 0xC2, 0x66, 0x4D, 0x52, 0x28, 0x1F, 0x3A};

    uint32_t *dst32 = mem_base_u32(g_mem_base, MM_PIF_MEM);
    uint32_t *src32 = (uint32_t*) pifimage;
    md5_state_t state;
    md5_byte_t digest[16];

    md5_init(&state);
    md5_append(&state, (const md5_byte_t*)pifimage, size);
    md5_finish(&state, digest);

    if (memcmp(digest, old_pif_ntsc_md5, 16) == 0 ||
        memcmp(digest, pif_ntsc_md5, 16) == 0)
    {
        DebugMessage(M64MSG_INFO, "Using NTSC PIF ROM");
    }
    else if (memcmp(digest, old_pif_pal_md5, 16) == 0 ||
             memcmp(digest, pif_pal_md5, 16) == 0)
    {
        DebugMessage(M64MSG_INFO, "Using PAL PIF ROM");
    }
    else
    {
        DebugMessage(M64MSG_ERROR, "Invalid PIF ROM");
        return M64ERR_INPUT_INVALID;
    }

    for (unsigned int i = 0; i < size; i += 4)
        *dst32++ = big32(*src32++);

    g_start_address = UINT32_C(0xbfc00000);
    return M64ERR_SUCCESS;
}

m64p_error close_pif(void)
{
    g_start_address = UINT32_C(0xa4000040);
    return M64ERR_SUCCESS;
}
