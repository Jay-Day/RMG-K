/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// macOS 26.2 added main-thread assertions to several NSOpenGLContext methods
// that interact with the view (setView:, update, clearDrawable).  Qt's
// QCocoaGLContext calls these inside makeCurrent() which is called from the
// emulation/render thread.
//
// Fix: swizzle each restricted method to dispatch_sync to the main thread when
// called from a background thread.  makeCurrentContext (the actual rendering
// binding) is NOT restricted and continues to run on the calling thread.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <dispatch/dispatch.h>

// ---- helpers ----------------------------------------------------------------

static void swizzle(Class cls, SEL original, SEL replacement)
{
    Method orig = class_getInstanceMethod(cls, original);
    Method repl = class_getInstanceMethod(cls, replacement);
    if (orig && repl)
        method_exchangeImplementations(orig, repl);
}

// ---- setView: ---------------------------------------------------------------

@interface NSOpenGLContext (RMGSetView)
- (void)rmg_setView:(NSView *)view;
@end

@implementation NSOpenGLContext (RMGSetView)
- (void)rmg_setView:(NSView *)view
{
    if ([NSThread isMainThread]) {
        [self rmg_setView:view];           // calls original after swizzle
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ [self rmg_setView:view]; });
    }
}
@end

// ---- update -----------------------------------------------------------------

@interface NSOpenGLContext (RMGUpdate)
- (void)rmg_update;
@end

@implementation NSOpenGLContext (RMGUpdate)
- (void)rmg_update
{
    if ([NSThread isMainThread]) {
        [self rmg_update];                 // calls original after swizzle
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ [self rmg_update]; });
    }
}
@end

// ---- clearDrawable ----------------------------------------------------------

@interface NSOpenGLContext (RMGClearDrawable)
- (void)rmg_clearDrawable;
@end

@implementation NSOpenGLContext (RMGClearDrawable)
- (void)rmg_clearDrawable
{
    if ([NSThread isMainThread]) {
        [self rmg_clearDrawable];          // calls original after swizzle
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ [self rmg_clearDrawable]; });
    }
}
@end

// ---- install ----------------------------------------------------------------

extern "C" void VidExtMac_InstallThreadSafeSetView(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = [NSOpenGLContext class];
        swizzle(cls, @selector(setView:),      @selector(rmg_setView:));
        swizzle(cls, @selector(update),        @selector(rmg_update));
        swizzle(cls, @selector(clearDrawable), @selector(rmg_clearDrawable));
    });
}
