#ifndef FRAMEBUFFER_TRANSITIONS_H
#define FRAMEBUFFER_TRANSITIONS_H

// RMG-K: frame buffer emulation for the game transitions that need it, for
// players who keep it off (see FrameBufferTransitions.cpp).

// Call at the start of each display list, before anything is drawn.
// _hiddenFrame: rollback re-simulates this frame and never shows it.
void FBTransitions_Update(bool _hiddenFrame);

// Gives the user's own frame buffer settings back. Call when the renderer
// stops or a ROM opens; the buffers are rebuilt from the settings anyway.
void FBTransitions_Reset();

// The settings were reloaded (config dialog): the forced state is gone.
void FBTransitions_SettingsReloaded();

// Call on each VI while frame buffer emulation is on. True: keep the picture
// on screen instead of showing the VI origin.
bool FBTransitions_KeepPicture(bool _originHasBuffer);

#endif // FRAMEBUFFER_TRANSITIONS_H
