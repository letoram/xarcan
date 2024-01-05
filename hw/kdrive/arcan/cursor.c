#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <cursorstr.h>

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

static arcanScrPriv *screenPtrArcan(ScreenPtr scr)
{
    KdScreenPriv(scr);
    KdScreenInfo *screen = pScreenPriv->screen;
    return screen->driver;
}

static void
arcanWarpCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    arcanScrPriv *scrpriv = screenPtrArcan(pScreen);
    scrpriv->cursorUpdated = true;
    miPointerWarpCursor(pDev, pScreen, x, y);
    scrpriv->cursor_event.ext.viewport.x = x;
    scrpriv->cursor_event.ext.viewport.y = y;
    arcanInputPriv.wx = x;
    arcanInputPriv.wy = y;
    if (scrpriv->cursor)
        arcan_shmif_enqueue(scrpriv->cursor, &scrpriv->cursor_event);
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

static Bool
mouseSpriteRealize(DeviceIntPtr dev, ScreenPtr screen, CursorPtr cursor)
{
    arcanScrPriv *scrpriv = screenPtrArcan(screen);
    scrpriv->cursorUpdated = true;
    scrpriv->cursorRealized = true;
    return TRUE;
}

static Bool
mouseSpriteUnrealize(DeviceIntPtr dev, ScreenPtr screen, CursorPtr cursor)
{
    arcanScrPriv *scrpriv = screenPtrArcan(screen);
    scrpriv->cursorUpdated = true;
    scrpriv->cursorRealized = false;
    return TRUE;
}

static void
mouseSpriteSet(DeviceIntPtr dev, ScreenPtr scr, CursorPtr cursor, int cx, int cy)
{
    arcanScrPriv *scrpriv = screenPtrArcan(scr);
    struct arcan_shmif_cont* ccon = scrpriv->cursor;
    if (!ccon || !ccon->addr)
        return;

   if (!cursor){
       scrpriv->cursorRealized = false;
       for (size_t y = 0; y < ccon->h; y++){
           memset(&ccon->vidp[y * ccon->pitch], '\0', ccon->stride);
       }
       arcan_shmif_signal(ccon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
       return;
    }

/* converge to the largest cursor size over time and just pad */
    if (cursor->bits->width > ccon->w || cursor->bits->height > ccon->h){
        trace("cursor-resize (%d, %d) -> (%d, %d)", ccon->w, ccon->h, cursor->bits->width, cursor->bits->height);
        arcan_shmif_resize(ccon, cursor->bits->width, cursor->bits->height);
        memset(ccon->vidp, '\0', ccon->h * ccon->stride);
    }

/* size to fit then blit and synch - trivial one */
    if (cursor->bits->argb){
        trace("argb-cursor (%d, %d)", cursor->bits->width, cursor->bits->height);
        for (size_t y = 0; y < ccon->h; y++){
            for (size_t x = 0; x < ccon->w; x++){
                shmif_pixel cc = SHMIF_RGBA(0, 0, 0, 0);
                if (x < cursor->bits->width && y < cursor->bits->height){
                     cc = cursor->bits->argb[y * cursor->bits->width + x];
                }
                ccon->vidp[y * ccon->pitch + x] = cc;
            }
        }
    }
    else {
/* more annoying: cursor->foreRed, foreGreen, foreBlue into one color, same for bg */
      shmif_pixel fg =
          SHMIF_RGBA(
                     cursor->foreRed,
                     cursor->foreGreen,
                     cursor->foreBlue, 0xff
          );

      shmif_pixel bg =
          SHMIF_RGBA(
                    cursor->backRed,
                    cursor->backGreen,
                    cursor->backBlue, 0xff
          );

      int stride = BitmapBytePad(cursor->bits->width);
      for (size_t y = 0; y < ccon->h; y++){
          for (size_t x = 0; x < ccon->w; x++){
              shmif_pixel cc = SHMIF_RGBA(0, 0, 0, 0);
              if (x < cursor->bits->width && y < cursor->bits->height){
                  int i = y * stride + x / 8;
                  int bit = 1 << (x & 7);
                  bool opaque = cursor->bits->mask[i] & bit;
                  if (opaque && cursor->bits->source[i] & bit)
                      cc = fg;
                  else if (opaque)
                      cc = bg;
              }

              ccon->vidp[y * ccon->pitch + x] = cc;
          }
      }
    }
    arcan_shmif_signal(ccon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

static void
mouseSpriteMove(DeviceIntPtr dev, ScreenPtr screen, int x, int y)
{
/*    trace("cursor(%d, %d)", x, y); */
}

static Bool
mouseSpriteInit(DeviceIntPtr dev, ScreenPtr screen)
{
    return true;
}

static void
mouseSpriteCleanup(DeviceIntPtr dev, ScreenPtr screen)
{
}

static miPointerSpriteFuncRec spriteFuncs =
{
    mouseSpriteRealize,
    mouseSpriteUnrealize,
    mouseSpriteSet,
    mouseSpriteMove,
    mouseSpriteInit,
    mouseSpriteCleanup
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

    if (!arcanInputPriv.pi->dixdev)
        return;

    miPointerGetPosition(arcanInputPriv.pi->dixdev, &x, &y);
    if (x == scr->cursor_event.ext.viewport.x &&
        y == scr->cursor_event.ext.viewport.y &&
        scr->cursorRealized != scr->cursor_event.ext.viewport.invisible)
        return;

    scr->cursor_event.ext.kind = ARCAN_EVENT(VIEWPORT);
    scr->cursor_event.ext.viewport.x = x;
    scr->cursor_event.ext.viewport.y = y;
    scr->cursor_event.ext.viewport.invisible = !scr->cursorRealized;

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
          arcan_shmif_enqueue(scr->cursor, &scr->cursor_event);
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
    if (arcanConfigPriv.soft_mouse){
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
