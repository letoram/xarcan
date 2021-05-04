/*
 * Copyright © 2004 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _ARCANDEV_H_
#define _ARCANDEV_H_

#include <stdio.h>
#include <unistd.h>
#include <arcan_shmif.h>

#include "kdrive.h"
#include "damage.h"

#ifdef RANDR
#include "randrstr.h"
#endif

typedef struct _arcanPriv {
    CARD8 *base;
    int bytes_per_line;
} arcanPriv;

struct pixmap_ext {
    struct gbm_bo *bo;
    void *image;
    unsigned int texture;
};

struct gbm_bo;
typedef struct _arcanScrPriv {
    struct arcan_shmif_cont * acon;
    struct arcan_shmif_initial init;
    Bool dirty;

#ifdef RANDR
    RROutputPtr randrOutput;
    RRCrtcPtr randrCrtc;
    struct ramp_block block;
#endif
    Rotation randr;
    ScreenBlockHandlerProcPtr BlockHandler;
    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseHandler;

    struct {
        PositionWindowProcPtr positionWindow;
        DestroyWindowProcPtr destroyWindow;
        RestackWindowProcPtr restackWindow;
        RealizeWindowProcPtr realizeWindow;
        UnrealizeWindowProcPtr unrealizeWindow;
        ChangeWindowAttributesProcPtr changeWindow;
        ConfigNotifyProcPtr configureWindow;
        GetImageProcPtr getImage;
    } hooks;

    ScreenPtr screen;
    DamagePtr damage;
    Bool in_glamor;
    struct gbm_bo* bo;
    int tex;
    int pending_fd;
    CARD16 stride;
    CARD32 size;
    int format;
} arcanScrPriv;

typedef struct _arcanInput {
    KdKeyboardInfo * ki;
    KdPointerInfo * pi;
} arcanInput;

typedef struct _arcanConfig {
    const char* title;
    const char* ident;
    Bool no_dri3;
    Bool glamor;
    Bool no_dynamic_resize;
    Bool txpass;
    Bool accel_cursor;
} arcanConfig;

extern int arcanGlamor;

extern arcanInput arcanInputPriv;

extern arcanConfig arcanConfigPriv;

extern KdCardFuncs arcanFuncs;

Bool
 arcanInitialize(KdCardInfo * card, arcanPriv * priv);

Bool
 arcanCardInit(KdCardInfo * card);

Bool
 arcanScreenInit(KdScreenInfo * screen);

Bool
 arcanScreenInitialize(KdScreenInfo * screen, arcanScrPriv * scrpriv);

Bool
 arcanInitScreen(ScreenPtr pScreen);

Bool
 arcanFinishInitScreen(ScreenPtr pScreen);

Bool
 arcanCreateResources(ScreenPtr pScreen);

void
arcanFlushEvents(int fd, void* tag);

int
arcanInit(void);

void
arcanFini(void);

void
 arcanScreenFini(KdScreenInfo * screen);

void
 arcanCardFini(KdCardInfo * card);

void
 arcanGetColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

void
 arcanPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

Bool
 arcanMapFramebuffer(KdScreenInfo * screen);

void *arcanWindowLinear(ScreenPtr pScreen,
                       CARD32 row,
                       CARD32 offset, int mode, CARD32 *size, void *closure);

void
 arcanSetScreenSizes(ScreenPtr pScreen);

Bool
 arcanUnmapFramebuffer(KdScreenInfo * screen);

Bool
 arcanSetShadow(ScreenPtr pScreen);

Bool
 arcanCreateColormap(ColormapPtr pmap);

#ifdef GLAMOR
Bool arcanGlamorInit(ScreenPtr screen);
void arcanGlamorEnable(ScreenPtr screen);
void arcanGlamorDisable(ScreenPtr screen);
void arcanGlamorFini(ScreenPtr screen);
#endif

/*
 * With RandR enabled, we treat the DISPLAYHINT events from parent
 * as a monitor- reconfigure.
 */
#ifdef RANDR
RRModePtr
arcan_cvt(int HDisplay, int VDisplay, float VRefresh, Bool Reduced,
             Bool Interlaced);
Bool
 arcanRandRGetInfo(ScreenPtr pScreen, Rotation * rotations);

Bool

arcanRandRSetConfig(ScreenPtr pScreen,
                   Rotation randr, int rate, RRScreenSizePtr pSize);
Bool
 arcanRandRInit(ScreenPtr pScreen);

#endif

void
 arcanCloseScreen(ScreenPtr pScreen);

extern KdPointerDriver arcanPointerDriver;

extern KdKeyboardDriver arcanKeyboardDriver;

#endif                          /* _FBDEV_H_ */
