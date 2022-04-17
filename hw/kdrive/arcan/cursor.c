#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include "arcan_cursor.h"

#ifdef DEBUG
#define ARCAN_TRACE
#endif
static inline void trace(const char* msg, ...)
{
#ifdef ARCAN_TRACE
    va_list args;
    va_start( args, msg );
        vfprintf(stderr,  msg, args );
        fprintf(stderr, "\n");
    va_end( args);
    fflush(stderr);
#endif
}

static void
arcanWarpCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

/* synching the cursor onwards is a pain, as we want to be able to skip
 * updating the composited surface when we have a sideband cursor, while at the
 * same time not saturate the queue by passing debt onwards or create feedback
 * loops through external warping */
    scrpriv->cursorUpdated = true;
    scrpriv->cx = x;
    scrpriv->cy = y;

    miPointerWarpCursor(pDev, pScreen, x, y);
}

static Bool
arcanCursorOffScreen(ScreenPtr *pScreen, int *x, int *y)
{
    return FALSE;
}

static void
arcanCursorCrossScreen(ScreenPtr pScreen, Bool entering)
{
}

static miPointerScreenFuncRec screenFuncs = {
    arcanCursorOffScreen,
    arcanCursorCrossScreen,
    arcanWarpCursor
};

static miPointerSpriteFuncRec spriteFuncs = {
/*
 * Realize
 * Unrealize
 * SetCursor
 * MoveCursor
 * Initialize
 * Cleanup
 */
};

static Status
MouseInit(KdPointerInfo * pi)
{
    arcanInputPriv.pi = pi;
    return Success;
}

static Status
MouseEnable(KdPointerInfo * pi)
{
    return Success;
}

static void
MouseDisable(KdPointerInfo * pi)
{
    return;
}

static void
MouseFini(KdPointerInfo * pi)
{
    return;
}

void arcanSynchCursor(arcanScrPriv *scr, Bool softUpdate)
{
    int x, y;
    miPointerGetPosition(arcanInputPriv.pi->dixdev, &x, &y);
    if (x == scr->cx && y == scr->cy)
        return;

    scr->cx = x;
    scr->cy = y;

/* the synch request comes from a context where it isn't determinable if there
 * are more in the queue to collate in order to reduce backpressure, mark the
 * screen as having a dirty cursor so that, at most, it will be synched on the
 * next frame or a shmif_wait->poll flush */
    if (softUpdate){
        scr->cursorUpdated = true;
        return;
    }

/* depending on if we have an accelerated cursor segment or a native one we
 * need to send different events - the cursorhint on the shared buffer or a
 * viewport with attachment on the cursor segment */
    scr->cursorUpdated = false;

    if (scr->cursor){
      struct arcan_event ev = (struct arcan_event)
                            {
                                .ext.kind = ARCAN_EVENT(VIEWPORT),
                                .ext.viewport = {
                                                    .x = x,
                                                    .y = y
                                                }
                            };
      arcan_shmif_enqueue(scr->cursor, &ev);
      return;
    }

    struct arcan_event ev = (struct arcan_event){
                                .ext.kind = ARCAN_EVENT(CURSORHINT)
                            };
    snprintf((char*)ev.ext.message.data, 78, "warp:%d:%d", x, y);
    arcan_shmif_enqueue(scr->acon, &ev);
}

Bool
arcanCursorInit(ScreenPtr screen)
{
/* this would make the Kdrive implementation take precedence and there
 * will be a soft-rasterized cursor into the composited output */
    if (!arcanConfigPriv.accel_cursor){
        trace("rejecting accelerated cursor");
        return FALSE;
    }

/* the 'TRUE' here sets waitForUpdate that is defined as having the
 * mi implementation defer cursor updates */
    miPointerInitialize(screen, &spriteFuncs, &screenFuncs, TRUE);

    return TRUE;
}

KdPointerDriver arcanPointerDriver = {
    "arcan",
    MouseInit,
    MouseEnable,
    MouseDisable,
    MouseFini,
    NULL
};
