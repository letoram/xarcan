#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <cursorstr.h>

/* cherry-picked and renamed from cursorfont.h */
#define Xcursor 0
#define Xleft_ptr 68
#define Xleft_side 70
#define Xwatch 150
#define Xterm 152
#define Xpirate 88
#define Xplus 90
#define Xtarget 128
#define Xtcross 130
#define Xcrosshair 30
#define Xquestion_arrow 92
#define Xhand1 58
#define Xhand2 60
#define Xtop_left_corner 134
#define Xtop_right_corner 136
#define Xtop_side 138
#define Xright_side 96
#define Xbottom_side 16
#define Xsb_h_double_arrow 108
#define Xsb_v_double_arrow 116
#define Xfleur 52
#define Xbottom_right_corner 14
#define Xbottom_left_corner 12
#define Xdraped_box 48
#define Xcircle 24

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include "arcan_cursor.h"

#ifdef DEBUG
#define ARCAN_TRACE
#endif

static void trace(const char* msg, ...)
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

#define send_cursor(name) do {\
        trace("send_cursor(%s)", name);\
        arcan_shmif_enqueue(ccon, &(arcan_event){\
            .ext.kind = ARCAN_EVENT(CURSORHINT),\
            .ext.message.data = name \
        }); } while (0);

/* Some clients will flip-flop cursors on and off between frames as a means of
 * implementing 'saveUnders' or flicker-free cursors. This means that we can't
 * blindly just forward cursor state. Instead check if the [cx, cy] from the
 * last sent viewport has changed from when it was last visible. This might not
 * be enough (i.e. autohide after a time), consider also having a frame-
 * counter as the reference state */
    if (!cursor){
        if (cx == scrpriv->cursor_event.ext.viewport.x &&
            cy == scrpriv->cursor_event.ext.viewport.y){
            return;
        }

        if (!scrpriv->cursor_event.ext.viewport.invisible){
            scrpriv->cursor_event.ext.viewport.invisible = true;
            arcan_shmif_enqueue(ccon, &scrpriv->cursor_event);
        }

        trace("SpriteSet(NULL):disable_cursor");
        scrpriv->cursorRealized = false;
        arcan_shmif_enqueue(ccon, &(arcan_event){
            .ext.kind = ARCAN_EVENT(CURSORHINT),
            .ext.message.data = "hidden"
        });

       return;
    }

/* if there is a matching glyph, prefer sending that instead */
    bool got_cursor = false;
    if (cursor->fromChar){
        got_cursor = true;

        switch (cursor->sourceChar){
/* generic */
        case Xtcross:
        case Xcrosshair: send_cursor("crosshair");    break;
        case Xcursor:
        case Xleft_ptr: send_cursor("default");       break;
        case Xwatch:    send_cursor("wait");          break;
        case Xpirate:   send_cursor("forbidden");     break;
        case Xquestion_arrow: send_cursor("help");    break;
        case Xhand1:
        case Xhand2:    send_cursor("hand");          break;
        case Xcircle:   send_cursor("forbidden");     break;
/* data */
        case Xterm:      send_cursor("typefield");    break;
        case Xtarget:   send_cursor("alias");         break;
        case Xplus:     send_cursor("cell");          break;
/* resize */
        case Xright_side:  send_cursor("east");       break;
        case Xtop_side:    send_cursor("north");      break;
        case Xbottom_side: send_cursor("south");      break;
        case Xleft_side:   send_cursor("west");       break;
        case Xtop_left_corner:
        case Xbottom_right_corner: send_cursor("north-west-south-east"); break;
        case Xsb_h_double_arrow:   send_cursor("east-west");             break;
        case Xsb_v_double_arrow:   send_cursor("north-south");           break;
        case Xfleur:               send_cursor("sizeall");               break;
        default:
        trace("unknown-glyph-id: %d", cursor->sourceChar);
        got_cursor = false;
        break;
        }
    }

/* we found a matching cursorfont entry, use that */
    if (got_cursor){
        if (!scrpriv->cursor_event.ext.viewport.invisible){
            scrpriv->cursor_event.ext.viewport.invisible = true;
            arcan_shmif_enqueue(ccon, &scrpriv->cursor_event);
        }
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
        arcan_shmif_signal(ccon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
    }
    else {
/* more annoying: cursor->foreRed, foreGreen, foreBlue into one color, same for bg */
        trace("rgb-cursor (%d, %d)", cursor->bits->width, cursor->bits->height);
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

      arcan_shmif_signal(ccon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
    }

    trace("cursor-signal");

/* mark as visible again so the hinted cursor won't be used */
    if (scrpriv->cursor_event.ext.viewport.invisible){
        trace("cursor-toggle-custom");
        scrpriv->cursor_event.ext.viewport.invisible = false;
        arcan_shmif_enqueue(ccon, &scrpriv->cursor_event);
    }
    else {
        if (scrpriv->cursor_event.ext.viewport.x != cx ||
            scrpriv->cursor_event.ext.viewport.y != cy){
            scrpriv->cursor_event.ext.viewport.x = cx;
            scrpriv->cursor_event.ext.viewport.y = cy;
            arcan_shmif_enqueue(ccon, &scrpriv->cursor_event);
        }
    }
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

/* don't synch if the position is the same as last */
    miPointerGetPosition(arcanInputPriv.pi->dixdev, &x, &y);
    if (x == scr->cursor_event.ext.viewport.x &&
        y == scr->cursor_event.ext.viewport.y)
        return;

    scr->cursor_event.ext.kind = ARCAN_EVENT(VIEWPORT);
    scr->cursor_event.ext.viewport.x = x;
    scr->cursor_event.ext.viewport.y = y;

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
