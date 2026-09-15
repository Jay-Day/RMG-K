/* mupen64plus-input-raphnetraw
 *
 * Copyright (C) 2016-2017 Raphael Assenat
 *
 * An input plugin that lets the game under emulation communicate with
 * the controllers directly using the direct controller communication
 * feature of raphnet V3 adapters[1].
 *
 * [1] http://www.raphnet.net/electronique/gcn64_usb_adapter_gen3/index_en.php
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * plugin_back.c : Plugin back code (in contrast to the "front" emulator interface
 *
 * Revision history:
 * 	28 Nov 2016 : Initial version
 * 	 1 Dec 2016 : Switch to block IO api
 *
 */

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#include "plugin_back.h"
#include "gcn64.h"
#include "gcn64lib.h"
#include "version.h"
#include "hexdump.h"

#undef ENABLE_TIMING

#ifdef ENABLE_TIMING
#include <sys/time.h>

#if defined(__MINGW32__) || defined(__MINGW64__)
#error Timing not supported under Windows
#endif
#endif

#undef TIME_RAW_IO
#undef TIME_COMMAND_TO_READ

#undef TRACE_BLOCK_IO
// Filter
#define EXTENSION_RW_ONLY 0

#define MAX_ADAPTERS	4
#define MAX_CHANNELS	4

static void nodebug(int l, const char *m, ...) { }

static pb_debugFunc DebugMessage = nodebug;



#define MAX_OPS	64

struct adapter {
	gcn64_hdl_t handle;
	struct gcn64_info inf;
	struct blockio_op biops[MAX_OPS];
	int n_ops;
};

static int g_n_adapters = 0;
struct adapter g_adapters[MAX_ADAPTERS] = { };

struct rawChannel {
	struct adapter *adapter;
	int chn;
};

struct cachedKeys {
	unsigned int keys;
	int valid;
};

/* Multiple adapters are supported, some are single player, others
 * two-player. As they are discovered during scan, their
 * channels (corresponding to physical controller ports) are added
 * to this table. Then, when plugin_front() calls functions in this
 * files with a specified channel, the channel is the index in this
 * table.
 */
static struct rawChannel g_channels[MAX_CHANNELS] = { };
static int g_n_channels = 0;
static volatile struct cachedKeys g_cached_keys[MAX_CHANNELS] = { };
static volatile int g_getKeys_polling = 0;
static int g_getKeys_thread_running = 0;
static int g_threading_initialized = 0;

static int pb_commandIsValid(int Control, unsigned char *Command);

static void pb_threadingInit(void);
static void pb_threadingShutdown(void);
static void pb_startGetKeysPolling(void);
static void pb_stopGetKeysPolling(void);
static void pb_mutexLockIo(void);
static void pb_mutexUnlockIo(void);

int pb_init(pb_debugFunc debugFn)
{
	DebugMessage = debugFn;
	pb_threadingInit();
	gcn64_init(1);
	return 0;
}

static void pb_freeAllAdapters(void)
{
	int i;

	pb_stopGetKeysPolling();

	for (i=0; i<g_n_adapters; i++) {
		if (g_adapters[i].handle) {
			/* RomClosed() should have done this, but just
			   in case it is not always called, do this again here. */
			gcn64lib_suspendPolling(g_adapters[i].handle, 0);
			gcn64_closeDevice(g_adapters[i].handle);
		}
	}

	g_n_channels = 0;
	g_n_adapters = 0;
	memset(g_adapters, 0, sizeof(g_adapters));
	memset(g_channels, 0, sizeof(g_channels));
}

int pb_shutdown(void)
{
	pb_freeAllAdapters();
	pb_threadingShutdown();
	gcn64_shutdown();

	return 0;
}

/**
 * \return The number of channels available.
 */
int pb_scanControllers(void)
{
	struct gcn64_list_ctx * lctx;
	int i, j;
	struct adapter *adap;

	lctx = gcn64_allocListCtx();
	if (!lctx) {
		DebugMessage(PB_MSG_ERROR, "Could not allocate gcn64 list context");
		return 0;
	}

	/* This may be called many times in the plugin's lifetime. For instance, each
	 * time a new game is selected from the PJ64 menu. Freeing previously found
	 * adapters here and creating a new list makes it possible to disconnect/replace
	 * USB adapters without having to restart PJ64. */
	pb_freeAllAdapters();

	/* Pass 1: Fill g_adapters[] with the adapters present on the system. */
	g_n_adapters = 0;
	adap = &g_adapters[g_n_adapters];
	while (gcn64_listDevices(&adap->inf, lctx)) {

		adap->handle = gcn64_openDevice(&adap->inf);
		if (!adap->handle) {
			DebugMessage(PB_MSG_ERROR, "Could not open gcn64 device serial '%ls'. Skipping it.", adap->inf.str_serial);
			continue;
		}

		DebugMessage(PB_MSG_INFO, "Found USB device 0x%04x:0x%04x serial '%ls' name '%ls'",
						adap->inf.usb_vid, adap->inf.usb_pid, adap->inf.str_serial, adap->inf.str_prodname);
		DebugMessage(PB_MSG_INFO, "Adapter supports %d raw channel(s)", adap->inf.caps.n_raw_channels);

		g_n_adapters++;
		if (g_n_adapters >= MAX_ADAPTERS)
			break;

		adap = &g_adapters[g_n_adapters];
	}
	gcn64_freeListCtx(lctx);

	/* Pass 2: Fill the g_channel[] array with the available raw channels.
	 * For instance, if there are adapters A, B and C (where A and C are single-player
	 * and B is dual-player), we get this:
	 *
	 * [0] = Adapter A, raw channel 0
	 * [1] = Adapter B, raw channel 0
	 * [2] = Adapter B, raw channel 1
	 * [3] = Adapter C, raw channel 0
	 *
	 * */
	for (i=0; i<g_n_adapters; i++) {
		struct adapter *adap = &g_adapters[i];

		if (adap->inf.caps.n_raw_channels <= 0)
			continue;

		for (j=0; j<adap->inf.caps.n_raw_channels; j++) {
			if (g_n_channels >= MAX_CHANNELS) {
				return g_n_channels;
			}
			g_channels[g_n_channels].adapter = adap;
			g_channels[g_n_channels].chn = j;
			DebugMessage(PB_MSG_INFO, "Channel %d: Adapter '%ls' raw channel %d", g_n_channels, adap->inf.str_serial, j);
			g_n_channels++;
		}
	}

	return g_n_channels;
}

static int g_input_mode = PB_INPUT_MODE_RAW_PIF;

void pb_setInputMode(int mode)
{
    if (mode != PB_INPUT_MODE_CACHED_GETKEYS)
    {
        mode = PB_INPUT_MODE_RAW_PIF;
    }
	
	
	if (g_input_mode == mode) {
        return;
    }

    if (mode == PB_INPUT_MODE_RAW_PIF) {
        pb_stopGetKeysPolling();
    }

    g_input_mode = mode;
}

int pb_getInputMode(void)
{
    return g_input_mode;
}

int pb_usesRawData(void)
{
    return g_input_mode == PB_INPUT_MODE_RAW_PIF;
}

int pb_romOpen(void)
{
	int i;
	
	for (i=0; i<MAX_ADAPTERS; i++) {
		if (g_adapters[i].handle) {
			gcn64lib_suspendPolling(g_adapters[i].handle, 1);
		}
	}
	
	pb_stopGetKeysPolling(); // safety

    if (g_input_mode == PB_INPUT_MODE_CACHED_GETKEYS)
    {
        pb_startGetKeysPolling();
    }

	return 0;
}

int pb_romClosed(void)
{
	int i;

	pb_stopGetKeysPolling();

	for (i=0; i<MAX_ADAPTERS; i++) {
		if (g_adapters[i].handle) {
			gcn64lib_suspendPolling(g_adapters[i].handle, 0);
		}
	}

	return 0;
}

#if defined(_WIN32)
static CRITICAL_SECTION g_io_mutex;
static HANDLE g_getKeys_thread = NULL;
#define PB_THREAD_RETURN DWORD WINAPI
#else
static pthread_mutex_t g_io_mutex;
static pthread_t g_getKeys_thread;
#define PB_THREAD_RETURN void *
#endif

static void pb_threadingInit(void)
{
	if (g_threading_initialized) {
		return;
	}

#if defined(_WIN32)
	InitializeCriticalSection(&g_io_mutex);
#else
	pthread_mutex_init(&g_io_mutex, NULL);
#endif
	g_threading_initialized = 1;
}

static void pb_threadingShutdown(void)
{
	if (!g_threading_initialized) {
		return;
	}

	pb_stopGetKeysPolling();

#if defined(_WIN32)
	DeleteCriticalSection(&g_io_mutex);
#else
	pthread_mutex_destroy(&g_io_mutex);
#endif
	g_threading_initialized = 0;
}

static void pb_mutexLockIo(void)
{
	pb_threadingInit(); // safety
	
#if defined(_WIN32)
	EnterCriticalSection(&g_io_mutex);
#else
	pthread_mutex_lock(&g_io_mutex);
#endif
}

static void pb_mutexUnlockIo(void)
{
#if defined(_WIN32)
	LeaveCriticalSection(&g_io_mutex);
#else
	pthread_mutex_unlock(&g_io_mutex);
#endif
}

static int pb_pollGetKeysOnce(int Control, unsigned int *Keys)
{
	unsigned char command[7] = { 0x01, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00 };
	unsigned char *rx = command + 3;
	struct rawChannel *channel;
	struct adapter *adap;
	struct blockio_op bio;
	int res;

	if (!Keys) {
		return 0;
	}

	*Keys = 0;

	if (!pb_commandIsValid(Control, command)) {
		return 0;
	}

	channel = &g_channels[Control];
	adap = channel->adapter;
	if (!adap || !adap->handle) {
		return 0;
	}

	memset(&bio, 0, sizeof(bio));
	bio.chn = channel->chn;
	bio.tx_len = command[0] & BIO_RXTX_MASK;
	bio.rx_len = command[1] & BIO_RXTX_MASK;
	bio.tx_data = command + 2;
	bio.rx_data = rx;

	pb_mutexLockIo();
	res = gcn64lib_blockIO(adap->handle, &bio, 1);
	pb_mutexUnlockIo();

	if (res != 0) {
		return 0;
	}

	if ((bio.rx_len & (BIO_RX_LEN_TIMEDOUT | BIO_RX_LEN_PARTIAL)) || ((bio.rx_len & BIO_RXTX_MASK) < 4)) {
		return 0;
	}

	*Keys = ((unsigned int)rx[0])
		| ((unsigned int)rx[1] << 8)
		| ((unsigned int)rx[2] << 16)
		| ((unsigned int)rx[3] << 24);
	return 1;
}

static PB_THREAD_RETURN pb_getKeysPollingThread(void *unused)
{
	int i;
	(void)unused;

	while (g_getKeys_polling) {
		for (i=0; i<g_n_channels && i<MAX_CHANNELS && g_getKeys_polling; i++) {
			unsigned int keys;
			if (pb_pollGetKeysOnce(i, &keys)) {
				g_cached_keys[i].keys = keys;
				g_cached_keys[i].valid = 1;
			}
		}
	}

#if defined(_WIN32)
	return 0;
#else
	return NULL;
#endif
}

static void pb_startGetKeysPolling(void)
{
	pb_threadingInit(); // safety
	
	if (g_getKeys_thread_running) {
		return;
	}

	memset(g_cached_keys, 0, sizeof(g_cached_keys));

	g_getKeys_polling = 1;

#if defined(_WIN32)
	g_getKeys_thread = CreateThread(NULL, 0, pb_getKeysPollingThread, NULL, 0, NULL);
	if (!g_getKeys_thread) {
		g_getKeys_polling = 0;
		DebugMessage(PB_MSG_ERROR, "Could not start GetKeys polling thread");
		return;
	}
#else
	if (pthread_create(&g_getKeys_thread, NULL, pb_getKeysPollingThread, NULL) != 0) {
		g_getKeys_polling = 0;
		DebugMessage(PB_MSG_ERROR, "Could not start GetKeys polling thread");
		return;
	}
#endif

	g_getKeys_thread_running = 1;
}

static void pb_stopGetKeysPolling(void)
{
	if (!g_getKeys_thread_running) {
		g_getKeys_polling = 0;
		return;
	}

	g_getKeys_polling = 0;

#if defined(_WIN32)
	WaitForSingleObject(g_getKeys_thread, INFINITE);
	CloseHandle(g_getKeys_thread);
	g_getKeys_thread = NULL;
#else
	pthread_join(g_getKeys_thread, NULL);
#endif

	g_getKeys_thread_running = 0;
}

#if defined(TIME_RAW_IO) || defined(TIME_COMMAND_TO_READ)
static void timing(int start, const char *label)
{
#ifdef ENABLE_TIMING
	static struct timeval tv_start;
	static int started = 0;
	struct timeval tv_now;

	if (start) {
		gettimeofday(&tv_start, NULL);
		started = 1;
		return;
	}

	if (started) {
		gettimeofday(&tv_now, NULL);
		printf("%s: %ld us\n", label, (tv_now.tv_sec - tv_start.tv_sec) * 1000000 + (tv_now.tv_usec - tv_start.tv_usec) );
		started = 0;
	}
#endif
}
#endif

#ifdef _DEBUG
static void debug_raw_commands_in(unsigned char *command, int channel_id)
{
	int tx_len = command[0];
	int rx_len = command[1] & 0x3F;
	int i;

	printf("chn %d: tx[%d] = {", channel_id, tx_len);
	for (i=0; i<tx_len; i++) {
		printf("0x%02x ", command[2+i]);
	}
	printf("}, expecting %d byte%c\n", rx_len, rx_len > 1 ? 's':' ');

}

static void debug_raw_commands_out(unsigned char *command, int channel_id)
{
	int result = command[1];
	int i;

	printf("chn %d: result: 0x%02x : ", channel_id, result);
	if (0 == (command[1] & 0xC0)) {
		for (i=0; i<result; i++) {
			printf("0x%02x ", command[2+i]);
		}
	} else {
		printf("error\n");
	}
	printf("\n");
}

#endif


int pb_controllerCommand(int Control, unsigned char *Command)
{
	// I thought of sending requests to the adapter from here, reading
	// the results later in readController.
	//
	// But unlike what I expected, controllerCommand is NOT always called
	// before readController... Will investigate later.
#ifdef TIME_COMMAND_TO_READ
	timing(1, NULL);
#endif
	return 0;
}

int pb_getKeys(int Control, unsigned int *Keys)
{
	unsigned char command[7] = { 0x01, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00 };
	int valid;
	
	// Normal path
    if (g_input_mode != PB_INPUT_MODE_CACHED_GETKEYS)
    {
        return pb_pollGetKeysOnce(Control, Keys);
    }

	if (!Keys) {
		return 0;
	}

	*Keys = 0;

	if (!pb_commandIsValid(Control, command)) {
		return 0;
	}

	// Cached path
	valid = g_cached_keys[Control].valid;
	if (valid) {
		*Keys = g_cached_keys[Control].keys;
	}

	return valid;
}

static int pb_performIo(void)
{
	struct adapter *adap;
	struct blockio_op *biops;
	int j, i, res;
#ifdef TRACE_BLOCK_IO
	int op_was_extio[MAX_OPS] = { };
#endif

	for (j=0; j<g_n_channels; j++)
	{
		adap = g_channels[j].adapter;
		biops = adap->biops;

		/* Skip adapters that do not have IO operations queued. */
		if (adap->n_ops <= 0)
			continue;

#ifdef TRACE_BLOCK_IO
		for (i=0; i<adap->n_ops; i++) {
			if (EXTENSION_RW_ONLY && (biops[i].tx_data[0] < 0x02))
				continue;
			op_was_extio[i] = 1;
			printf("Before blockIO: op %d, chn: %d, : tx: 0x%02x, rx: 0x%02x, data: ", i, biops[i].chn,
						biops[i].tx_len, biops[i].rx_len);
			printHexBuf(biops[i].tx_data, biops[i].tx_len);
		}
#endif

#ifdef TIME_RAW_IO
		timing(1, NULL);
#endif
		pb_mutexLockIo();
		res = gcn64lib_blockIO(adap->handle, biops, adap->n_ops);
		pb_mutexUnlockIo();
#ifdef TIME_RAW_IO
		timing(0, "blockIO");
#endif

		if (res == 0) {
			// biops rx_data pointed into PIFram so data is there. But we need
			// to patch the RX length parameters (the two most significant bits
			// are error bits such as timeout..)
			for (i=0; i<adap->n_ops; i++) {
				// in PIFram, the read length is one byte before the tx data. A rare
				// occasion to use a negative array index ;)
				biops[i].tx_data[-1] = biops[i].rx_len;

#ifdef TRACE_BLOCK_IO
				if (EXTENSION_RW_ONLY && (!op_was_extio[i])) // Read
					continue;

				printf("After blockIO: op %d, chn: %d, : tx: 0x%02x, rx: 0x%02x, data: ", i, biops[i].chn,
						biops[i].tx_len, biops[i].rx_len);
				if (biops[i].rx_len & BIO_RX_LEN_TIMEDOUT) {
					printf("Timeout\n");
				} else if (biops[i].rx_len & BIO_RX_LEN_PARTIAL) {
					printf("Incomplete\n");
				} else {
					printHexBuf(biops[i].rx_data, biops[i].rx_len);
				}
#endif
			}
		} else {
			// For debugging
			//exit(1);
		}

		adap->n_ops = 0;
	}
	return 0;


}

static int pb_commandIsValid(int Control, unsigned char *Command)
{
	// A negative Control is genuinely malformed and worth flagging.
	if (Control < 0) {
		DebugMessage(PB_MSG_WARNING, "pb_readController called with Control=%d", Control);
		return 0;
	}

	// Control >= g_n_channels means the PIF is polling a slot beyond what
	// this adapter has channels for. InitiateControllers already declared
	// those slots Present=0, so the poll happening is expected hardware
	// behavior (N64 PIF polls all 4 slots every frame) — not an error.
	if (Control >= g_n_channels) {
		return 0;
	}

	if (!Command) {
		DebugMessage(PB_MSG_WARNING, "pb_readController called with NULL Command pointer");
		return 0;
	}

	// When a CIC challenge took place in update_pif_write(), the pif ram
	// contains a bunch 0xFF followed by 0x00 at offsets 46 and 47. Offset
	// 48 onwards contains the challenge answer.
	//
	// Then when update_pif_read() is called, the 0xFF bytes are be skipped
	// up to the two 0x00 bytes that increase the channel to 2. Then the
	// challenge answer is (incorrectly) processed as if it were commands
	// for the third controller.
	//
	// This cause issues with the raphnetraw plugin since it modifies pif ram
	// to store the result or command error flags. This corrupts the reponse
	// and leads to challenge failure.
	//
	// As I know of no controller commands above 0x03, the filter below guards
	// against this condition...
	//
	if (Control == 2 && Command[2] > 0x03) {
		DebugMessage(PB_MSG_WARNING, "Invalid controller command");
		return 0;
	}

	// When Mario Kart 64 uses a controller pak, such PIF ram content
	// occurs:
	//
	// ff 03 21 02 01 f7 ff ff
	// ff ff ff ff ff ff ff ff
	// ff ff ff ff ff ff ff ff
	// ff ff ff ff ff ff ff ff
	// ff ff ff ff ff ff ff 21
	// fe 00 00 00 00 00 00 00
	// 00 00 00 00 00 00 00 00
	// 00 00 00 00 00 00 00 00
	//
	// It results in this:
	//  - Transmission of 3 bytes with a 33 bytes return on channel 0,
	//  - Transmission of 33 bytes with 254 bytes in return on channel 1!?
	//
	// Obviously the second transaction is an error. The 0xFE (254) that follows
	// is where processing should actually stop. This happens to be an invalid length detectable
	// by looking at the two most significant bits..
	//
	if (Command[0] == 0xFE && Command[1] == 0x00) {
		DebugMessage(PB_MSG_WARNING, "Ignoring invalid io operation (T: 0x%02x, R: 0x%02x)",
			Command[0], Command[1]);
		return 0;
	}

	return 1;
}

int pb_readController(int Control, unsigned char *Command)
{
	struct rawChannel *channel;
	struct adapter *adap;
	struct blockio_op *biops;

	// Called with -1 at the end of PIF ram.
	if (Control == -1) {
		return pb_performIo();
	}

	/* Check for out of bounds Control parameter, for
	 * NULL Command and filter various invalid conditions. */
	if (!pb_commandIsValid(Control, Command)) {
		return 0;
	}

	/* Add the IO operation to the block io list of
	 * the adapter serving this channel. */

	channel = &g_channels[Control];
	adap = channel->adapter;

	if (adap->n_ops >= MAX_OPS) {
		DebugMessage(PB_MSG_ERROR, "Too many io ops");
	} else {
		biops = adap->biops;

		biops[adap->n_ops].chn = channel->chn; // Control;
		biops[adap->n_ops].tx_len = Command[0] & BIO_RXTX_MASK;
		biops[adap->n_ops].rx_len = Command[1] & BIO_RXTX_MASK;
		biops[adap->n_ops].tx_data = Command + 2;
		biops[adap->n_ops].rx_data = Command + 2 + biops[adap->n_ops].tx_len;

		if (biops[adap->n_ops].tx_len == 0 || biops[adap->n_ops].rx_len == 0) {
			DebugMessage(PB_MSG_WARNING, "TX or RX was zero");
			return 0;
		}

		adap->n_ops++;
	}

	return 0;
}
