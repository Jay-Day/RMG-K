// RMG-K: many players keep frame buffer emulation off for its lower display
// delay. With it off GLideN64 never writes what it draws back to RDRAM, so a
// game that copies its last frame with the CPU and shows the copy gets black.
// For the games below, frame buffer emulation is turned on just around such a
// copy and off again. The user's settings never change: the forced state lives
// here, and the config dialog and the ini only ever see the user's own values.

#include <atomic>
#include "FrameBufferTransitions.h"
#include "Config.h"
#include "N64.h"
#include "gDP.h"
#include "gSP.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "FrameBufferInfo.h"
#include "Textures.h"
#include "DisplayWindow.h"

// Super Smash Bros. (U) and Smash Remix, which is built on it and keeps these.
// The VS results scene starts by copying the frame on screen out of RDRAM
// (lbTransitionSetupTransition) and animates that copy away. GAME SET (or TIME
// UP) sets the battle's game status to End, then BossDefeat for 90 frames, then
// Set for 3 frames before the scene ends.
static const u32 kSmashSceneCurrent = 0x800A4AD0;	// u8 gSCManagerSceneData.scene_curr
static const u32 kSmashBattleState = 0x800A50E8;	// SCBattleState * gSCManagerBattleState
static const u32 kSmashGameStatus = 0x11;			// u8 SCBattleState.game_status
static const u8 kSmashSceneVSBattle = 0x16;
static const u8 kSmashStatusEnd = 5;
static const u8 kSmashStatusSet = 7;

// A rollback can show a GAME SET that never happens for up to its prediction
// window (10 frames at most), so the switch waits for twice that. That still
// leaves about 70 frames before the copy is taken.
static const u32 kEndFramesBeforeOn = 20;
// In case a battle stays ended without leaving the scene.
static const u32 kEndFramesMax = kEndFramesBeforeOn + 600;

static bool s_forced = false;
static u32 s_userCopyToRDRAM = 0;
static u32 s_endFrames = 0;
static u32 s_keepPictureVIs = 0;
static std::atomic<bool> s_settingsReloaded(false);

static u8 _readU8(u32 _address)
{
	_address &= 0x1FFFFFFF;
	if (_address > RDRAMSize)
		return 0;
	return RDRAM[_address ^ 3];
}

static u32 _readU32(u32 _address)
{
	_address &= 0x1FFFFFFC;
	if (_address + 3 > RDRAMSize)
		return 0;
	return *reinterpret_cast<u32*>(RDRAM + _address);
}

// On from shortly after GAME SET until the next scene draws its first frame:
// the results scene has taken its copy by then, and the copy is plain RDRAM.
static bool _smashNeedsFrameBuffer()
{
	bool ending = false;
	if (_readU8(kSmashSceneCurrent) == kSmashSceneVSBattle) {
		const u32 battle = _readU32(kSmashBattleState);
		const u8 status = battle != 0 ? _readU8(battle + kSmashGameStatus) : 0;
		ending = status >= kSmashStatusEnd && status <= kSmashStatusSet;
	}
	if (!ending) {
		s_endFrames = 0;
		return false;
	}
	if (s_endFrames < kEndFramesMax)
		++s_endFrames;
	return s_endFrames >= kEndFramesBeforeOn && s_endFrames < kEndFramesMax;
}

static bool _canForce()
{
	return config.frameBufferEmulation.enable == 0 &&
		config.frameBufferEmulation.enableForTransitions != 0 &&
		// N64-style depth compare can't work with multisampled buffers (Config::validate)
		(config.frameBufferEmulation.N64DepthCompare == Config::dcDisable || config.video.multisampling == 0);
}

// The frame and depth buffers are made for one mode. Start over with new ones,
// as at ROM start: the compiled shaders and everything else don't depend on the
// mode and stay.
static void _rebuildBuffers()
{
	GraphicsDrawer & drawer = dwnd().getDrawer();
	drawer.flush();
	drawer.dropRenderState();
	FrameBuffer_Destroy();
	DepthBuffer_Destroy();
	DepthBuffer_Init();
	FrameBuffer_Init();
	FBInfo::fbInfo.reset();
	// the old buffers took their textures with them
	textureCache().current[0] = textureCache().current[1] = nullptr;
	for (u32 i = 0; i < 8; ++i) {
		if (gDP.tiles[i].textureMode == TEXTUREMODE_FRAMEBUFFER || gDP.tiles[i].textureMode == TEXTUREMODE_FRAMEBUFFER_BG)
			gDP.tiles[i].textureMode = TEXTUREMODE_NORMAL;
	}
	gSP.changed = gDP.changed = 0xFFFFFFFF;
}

static void _force(bool _on)
{
	if (_on) {
		s_userCopyToRDRAM = config.frameBufferEmulation.copyToRDRAM;
		config.frameBufferEmulation.enable = 1;
		// the copy the game takes has to be the frame it shows
		config.frameBufferEmulation.copyToRDRAM = Config::ctSync;
		s_keepPictureVIs = 3;
	} else {
		config.frameBufferEmulation.enable = 0;
		config.frameBufferEmulation.copyToRDRAM = s_userCopyToRDRAM;
		s_keepPictureVIs = 0;
	}
	s_forced = _on;
	_rebuildBuffers();
}

void FBTransitions_Update(bool _hiddenFrame)
{
	bool rebuild = false;
	if (s_settingsReloaded.exchange(false) && s_forced) {
		// the dialog loaded the user's settings back, the buffers are not for them
		s_forced = false;
		rebuild = true;
	}

	// Only frames that are shown count: rollback re-simulates frames many
	// times, and a mispredicted GAME SET shows only until the rollback.
	if ((config.generalEmulation.hacks & hack_SmashTransitions) != 0 && !_hiddenFrame) {
		const bool needed = _smashNeedsFrameBuffer();
		if (needed != s_forced && (!needed || _canForce())) {
			_force(needed);
			return;
		}
	}

	if (rebuild)
		_rebuildBuffers();
}

void FBTransitions_Reset()
{
	if (s_forced) {
		config.frameBufferEmulation.enable = 0;
		config.frameBufferEmulation.copyToRDRAM = s_userCopyToRDRAM;
		s_forced = false;
	}
	s_endFrames = 0;
	s_keepPictureVIs = 0;
	s_settingsReloaded = false;
}

void FBTransitions_SettingsReloaded()
{
	s_settingsReloaded = true;
}

// Right after the switch on, the frame on screen was drawn without frame
// buffer emulation: it has no buffer, and its RDRAM holds an old picture.
bool FBTransitions_KeepPicture(bool _originHasBuffer)
{
	if (s_keepPictureVIs == 0)
		return false;
	if (_originHasBuffer) {
		s_keepPictureVIs = 0;
		return false;
	}
	--s_keepPictureVIs;
	return true;
}
