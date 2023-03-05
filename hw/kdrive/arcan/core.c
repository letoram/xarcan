/*
 * The kdrive bits have slowly been migrated away from, can almost
 * stop using it entirely soon.
 *
 * Rough notes on what it is missing:
 *
 *  - the order value for reconstructing composition hierarch is not correct
 *
 *  - to get better .utf8 handling for input, consider injecting us as an XIM
 *    and use that to provide the actual text.
 *
 *  - present clients not handled
 *
 *  - on restacking a window, check if its parent is redirected and if so,
 *    redirect itself - and if it was, check if it still should be
 *
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include <X11/keysym.h>
#include <pthread.h>
#include <xcb/xcb.h>
#include <sys/wait.h>
#include "compint.h"

#ifdef GLAMOR
#define EGL_NO_X11
#include <gbm.h>
#include "glamor.h"
#include "glamor_context.h"
#include "glamor_egl.h"
#include "dri3.h"
#include <drm_fourcc.h>
#endif

#include "inputstr.h"
#include "inpututils.h"
#include "micmap.h"

#define CURSOR_REQUEST_ID 0xfef0
#define CLIPBOARD_REQUEST_ID 0xfafa

/*
 * from Xwin
 */
#include <opaque.h>
#define XSERV_t
#define TRANS_SERVER
#include <X11/Xtrans/Xtrans.h>
#include <present.h>
#include "../../dix/enterleave.h"

arcanInput arcanInputPriv;
arcanConfig arcanConfigPriv;
int arcanGlamor;
static int mouseButtonBitmap;

#ifdef __OpenBSD__
#include "../../dmx/input/atKeynames.h"
#include "bsd_KbdMap.c"

static void enqueueKeyboard(uint16_t scancode, int active)
{
    KdEnqueueKeyboardEvent(arcanInputPriv.ki, wsUsbMap[scancode], !active);
}
#else
static void enqueueKeyboard(uint16_t scancode, int active)
{
    KdEnqueueKeyboardEvent(arcanInputPriv.ki, scancode, !active);
}
#endif

static uint8_t code_tbl[512];
static struct arcan_shmif_initial* arcan_init;
static DevPrivateKeyRec pixmapPriv;
static DevPrivateKeyRec windowPriv;
static void
synchTreeDepth(arcanScrPriv* scrpriv, WindowPtr wnd,
      void (*callback)(WindowPtr node, int depth, int max_depth, void* tag),
         bool force,
         void* tag);

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

#if 0
static struct {
    Atom _NET_WM_WINDOW_TYPE;
    Atom _NET_WM_WINDOW_TYPE_DESKTOP;
    Atom _NET_WM_WINDOW_TYPE_DOCK;
    Atom _NET_WM_WINDOW_TYPE_COMBO;
    Atom _NET_WM_WINDOW_TYPE_DND;
    Atom _NET_WM_WINDOW_TYPE_DROPDOWN_MENU;
    Atom _NET_WM_WINDOW_TYPE_POPUP_MENU;
    Atom _NET_WM_WINDOW_TYPE_TOOLBAR;
    Atom _NET_WM_WINDOW_TYPE_MENU;
    Atom _NET_WM_WINDOW_TYPE_UTILITY;
    Atom _NET_WM_WINDOW_TYPE_SPLASH;
    Atom _NET_WM_WINDOW_TYPE_DIALOG;
    Atom _NET_WM_WINDOW_TYPE_TOOLTIP;
    Atom _NET_WM_WINDOW_TYPE_NOTIFICATION;
    Atom _NET_WM_WINDOW_TYPE_NORMAL;
} wm_atoms;

static void resolve_atoms(void)
{
#define make(N) MakeAtom(N, strlen(N), false)
    wm_atoms._NET_WM_WINDOW_TYPE = make("_NET_WM_WINDOW_TYPE");
  wm_atoms._NET_WM_WINDOW_TYPE_DESKTOP = make("_NET_WM_WINDOW_TYPE_DESKTOP");
  wm_atoms._NET_WM_WINDOW_TYPE_DOCK = make("_NET_WM_WINDOW_TYPE_DOCK");
  wm_atoms._NET_WM_WINDOW_TYPE_COMBO = make("_NET_WM_WINDOW_TYPE_COMBO");
  wm_atoms._NET_WM_WINDOW_TYPE_DND = make("_NET_WM_WINDOW_TYPE_DND");
  wm_atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU = make("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  wm_atoms._NET_WM_WINDOW_TYPE_POPUP_MENU = make("_NET_WM_WINDOW_TYPE_POPUP_MENU");
  wm_atoms._NET_WM_WINDOW_TYPE_TOOLBAR = make("_NET_WM_WINDOW_TYPE_TOOLBAR");
  wm_atoms._NET_WM_WINDOW_TYPE_MENU = make("_NET_WM_WINDOW_TYPE_MENU");
  wm_atoms._NET_WM_WINDOW_TYPE_UTILITY = make("_NET_WM_WINDOW_TYPE_UTILITY");
  wm_atoms._NET_WM_WINDOW_TYPE_SPLASH = make("_NET_WM_WINDOW_TYPE_SPLASH");
  wm_atoms._NET_WM_WINDOW_TYPE_DIALOG = make("_NET_WM_WINDOW_TYPE_DIALOG");
  wm_atoms._NET_WM_WINDOW_TYPE_TOOLTIP = make("_NET_WM_WINDOW_TYPE_TOOLTIP");
  wm_atoms._NET_WM_WINDOW_TYPE_NOTIFICATION = make("_NET_WM_WINDOW_TYPE_NOTIFICATION");
  wm_atoms._NET_WM_WINDOW_TYPE_NORMAL = make("_NET_WM_WINDOW_TYPE_NORMAL");
#undef make
}
#endif

static void applyArcanRedirect(arcanScrPriv *S,
                               WindowPtr wnd, struct arcan_shmif_cont *C, uint64_t i);

/*
 *  mostly a precaution against future uses of multiple threads sending events
 *  at the same time if more messages are added from the proxyWnd
 */
static void ARCAN_ENQUEUE(struct arcan_shmif_cont* c, struct arcan_event* e)
{
    arcan_shmif_lock(c);
        arcan_shmif_enqueue(c, e);
    arcan_shmif_unlock(c);
}

/*
 * likely that these calculations are incorrect, need BE machines to
 * test between both setArcan and setGlamor versions, stick with the
 * setGlamor version for now.
 */
static void setArcanMask(KdScreenInfo* screen)
{
    screen->rate = arcan_init->rate;
    screen->fb.depth = 24;
    screen->fb.bitsPerPixel = 32;
    screen->fb.visuals = (1 << TrueColor) | (1 << DirectColor);
    screen->fb.redMask = SHMIF_RGBA(0xff, 0x00, 0x00, 0x00);
    screen->fb.greenMask = SHMIF_RGBA(0x00, 0xff, 0x00, 0x00);
    screen->fb.blueMask = SHMIF_RGBA(0x00, 0x00, 0xff, 0x00);
/*        miSetVisualTypesAndMasks(
            32, (1 << TrueColor) | (1 << DirectColor),
            8, TrueColor, screen->fb.redMask, screen->fb.greenMask, screen->fb.blueMask);*/
 }

static void setGlamorMask(KdScreenInfo* screen)
{
    int bpc, green_bpc;
    screen->rate = arcan_init->rate;
    screen->fb.visuals = (1 << TrueColor) | (1 << DirectColor);
    screen->fb.depth = 24;
/* calculations used in xWayland and elsewhere */
    bpc = 24 / 3;
    green_bpc = 24 - 2 * bpc;
    screen->fb.bitsPerPixel = 32;
    screen->fb.blueMask = (1 << bpc) - 1;
    screen->fb.greenMask = ((1 << green_bpc) - 1) << bpc;
    screen->fb.redMask = screen->fb.blueMask << (green_bpc + bpc);
}

Bool
arcanInitialize(KdCardInfo * card, arcanPriv * priv)
{
    trace("arcanInitialize");
    priv->base = 0;
    priv->bytes_per_line = 0;
    return TRUE;
}

Bool
arcanCardInit(KdCardInfo * card)
{
    arcanPriv *priv;
    trace("arcanCardInit");

    priv = (arcanPriv *) malloc(sizeof(arcanPriv));
    if (!priv)
        return FALSE;

    if (!arcanInitialize(card, priv)) {
        free(priv);
        return FALSE;
    }
    card->driver = priv;

    return TRUE;
}

static arcanScrPriv* getArcanScreen(WindowPtr wnd)
{
    ScreenPtr pScreen = wnd->drawable.pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    return scrpriv;
 }

static void updateDirty(struct arcan_shmif_cont *C, DamagePtr damage)
{
    RegionPtr region = DamageRegion(damage);
    if (!RegionNotEmpty(region))
        return;

    BoxPtr box = RegionExtents(region);
    arcan_shmif_dirty(C, box->x1, box->y1, box->x2, box->y2, 0);
}

static bool isRedirectCandidate(arcanScrPriv *ascr, WindowPtr wnd)
{
    return wnd->parent && wnd->parent == ascr->screen->root && wnd->drawable.class == InputOutput;
}

static void arcanGetImage(DrawablePtr pDrawable, int sx, int sy, int w, int h,
                          unsigned int format, unsigned long planeMask, char *pdstLine)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *ascr = screen->driver;
    if (pScreen->DispatchReqSrc)
        return;

    trace("arcanGetImage");
    if (ascr->hooks.getImage){
        ascr->screen->GetImage = ascr->hooks.getImage;
        ascr->hooks.getImage(pDrawable,
                             sx, sy, w, h, format, planeMask, pdstLine);
        ascr->screen->GetImage = arcanGetImage;
    }
}

static DrawablePtr arcanDrawableInterposeSrcDst(ClientPtr cl, DrawablePtr src, DrawablePtr dst)
{
/* blocking this entirely will break clients that have other scratch surfaces,
 * the only real interpositioning we can do is when src comes from a protected
 * surface and maybe when dst is in an XSHM pixmap, but it doesn't have a
 * distinguishing type (just shmdesc->addr + offset) */
    return src;
}

static PixmapPtr arcanGetWindowPixmap(WindowPtr wnd)
{
    ScreenPtr pScreen = wnd->drawable.pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr res = NULL;

/*
 * for (size_t i = 0; i < 64; i++){
      if (!scrpriv->redirectSegments[i].C)
        continue;
      if (wnd == scrpriv->redirectSegments[i].C->user){
        trace("return intercept");
        return scrpriv->redirectSegments[i].pmap;
      }
    }
 */

/* Reject or substitute DispatchReqScr if the window has been marked as
 * protected. */
    if (1 || (scrpriv->hooks.getWindowPixmap && !pScreen->DispatchReqSrc)){
        scrpriv->screen->GetWindowPixmap = scrpriv->hooks.getWindowPixmap;
        res = scrpriv->hooks.getWindowPixmap(wnd);
        scrpriv->screen->GetWindowPixmap = arcanGetWindowPixmap;
    }

    return res;
}

static bool isWindowVisible(WindowPtr wnd)
{
    return wnd->mapped &&
           wnd->realized &&
           wnd->visibility != VisibilityNotViewable &&
           wnd->visibility != VisibilityFullyObscured;
}

static void flagUnsynched(WindowPtr wnd)
{
    while (wnd && wnd->parent){
        wnd->unsynched = 1;
        wnd = wnd->parent;
    }
}

static void copyDrawableShmif(
    arcanScrPriv *ascr,
    struct redirectMeta* M, PixmapPtr pmap,
    struct arcan_shmif_region dirty)
{
    DrawablePtr src = &pmap->drawable;
    GCPtr pGC = GetScratchGC(src->depth, ascr->screen);
    if (!pGC)
        return;

    trace("redirect-region: %zu, %zu, %zu, %zu (origin: %d, %d)",
           dirty.x1, dirty.y1, dirty.x2, dirty.y2, src->x, src->y);
    ChangeGCVal val;
    val.val = IncludeInferiors;
    ChangeGC(NullClient, pGC, GCSubwindowMode, &val);
    ValidateGC(src, pGC);

/* slide the dest around so that it gets copied at the right position */
    pmap->drawable.x = src->x;
    pmap->drawable.y = src->y;

    (void) (*pGC->ops->CopyArea)(src,
                                 &M->pmap->drawable, pGC,
                                 dirty.x1,  dirty.y1,
                                 dirty.x2 - dirty.x1,
                                 dirty.y2 - dirty.y1,
                                 dirty.x1,  dirty.y1);
    FreeScratchGC(pGC);
}

static void sendWndData(WindowPtr wnd, int depth, int max_depth, void* tag)
{
    struct arcan_shmif_cont* acon = tag;

/* might be better to defer this and just mark the window for update, add
 * to a list and do in the blockHandler previous to signalling so that we
 * don't saturate the event-queue with a pile-up */
    int bw = wnd->borderWidth;

/* the stacking order could be reconstructed through miXYToWindow with
 * the pSprite structure from miXYToWindow actually */
    struct arcan_event out = (struct arcan_event)
    {
       .category = EVENT_EXTERNAL,
       .ext.kind = ARCAN_EVENT(VIEWPORT),
       .ext.viewport.x = wnd->drawable.x - bw,
       .ext.viewport.y = wnd->drawable.y - bw,
       .ext.viewport.w = wnd->drawable.width + bw + bw,
       .ext.viewport.h = wnd->drawable.height + bw + bw,
       .ext.viewport.border = {bw, bw, bw, bw},
       .ext.viewport.parent = wnd->parent ? wnd->parent->drawable.id : 0,
       .ext.viewport.embedded = !(wnd->parent && !wnd->parent->parent),
       .ext.viewport.ext_id = wnd->drawable.id,
       .ext.viewport.order = wnd->visibility == VisibilityUnobscured ? 0 : -1,
       .ext.viewport.invisible = !isWindowVisible(wnd),
       .ext.viewport.focus = EnterLeaveWindowHasFocus(wnd)
    };

/* other things to track and send here would be:
 * wUserProps(win); PropertyPtr; (linked list), then get XA_WM_NAME and
 * check that iti is XA_STRING with prop>data and send */

/* actually only send if the contents differ from what we sent last time */
    arcanWndPriv *awnd = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    if (awnd){
        if (memcmp(&out, &awnd->ev, sizeof(struct arcan_event)) != 0){
            memcpy(&awnd->ev, &out, sizeof(struct arcan_event));
            ARCAN_ENQUEUE(acon, &awnd->ev);
        }
    }

    wnd->unsynched = 0;
}

static void resetWndData(WindowPtr wnd, int depth, int max_depth, void* tag)
{
    struct arcan_shmif_cont* acon = tag;
    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE),
    };
    snprintf(
             (char*)ev.ext.message.data, 78,
             "kind=create:xid=%d:parent=%d:next=%d",
             (int) wnd->drawable.id,
             wnd->parent ? (int) wnd->parent->drawable.id : -1,
             wnd->nextSib ? (int) wnd->nextSib->drawable.id : -1
    );
    ARCAN_ENQUEUE(acon, &ev);
    flagUnsynched(wnd);
}

static Bool arcanPositionWindow(WindowPtr wnd, int x, int y)
{
    Bool rv = true;
    arcanScrPriv *ascr = getArcanScreen(wnd);
    if (ascr->hooks.positionWindow){
        ascr->screen->PositionWindow = ascr->hooks.positionWindow;
        rv = ascr->hooks.positionWindow(wnd, x, y);
        ascr->screen->PositionWindow = arcanPositionWindow;
    }

    flagUnsynched(wnd);
    return rv;
}

static Bool arcanMarkOverlapped(WindowPtr pwnd, WindowPtr firstchild, WindowPtr *layer)
{
    arcanScrPriv *ascr = getArcanScreen(pwnd);

     Bool rv = true;
     if (ascr->hooks.markOverlappedWindows){
        ascr->screen->MarkOverlappedWindows = ascr->hooks.markOverlappedWindows;
        rv = ascr->screen->MarkOverlappedWindows(pwnd, firstchild, layer);
        ascr->screen->MarkOverlappedWindows = arcanMarkOverlapped;
      }

    flagUnsynched(pwnd);
    flagUnsynched(firstchild);
    return rv;
}

static struct redirectMeta*
redirectWindow(arcanScrPriv *S, WindowPtr wnd, struct arcan_shmif_cont *C)
{
/* add to tracking bitmap, should perhaps chunk this into groups and cap at a k windows or sth */
    uint64_t i = __builtin_ffsll(~S->redirectBitmap);
    if (0 == i){
        arcan_shmif_drop(C);
        return NULL;
    }
    i--;
    applyArcanRedirect(S, wnd, C, i);

/* If we got glamor / dri3 then accelerate the segment as well and n-buffer */
    return &S->redirectSegments[i];
}

static struct redirectMeta *findFreeSegment(arcanScrPriv *ascr, WindowPtr wnd, bool blockAlloc)
{
/* all ffsll calls should be chased / abstracted into allocation-group + ptr,
 * 64 is perhaps to few but 64 * 64 more than useful */
    uint64_t bmap = ascr->redirectBitmap;
    struct redirectMeta *best = NULL;

/* step each redirection, find the unused but allocated */
    while (bmap){
        uint64_t i = __builtin_ffsll(bmap) - 1;
        struct arcan_shmif_cont* C = ascr->redirectSegments[i].C;
        bmap &= ~(1ull << i);
        if (!C || ascr->redirectSegments[i].redirected)
            continue;

/* improvement here is to compare area of wnd to area of bmap, and prefer
 * best match or within resize tolerance to avoid a resize roundtrip */
        best = &ascr->redirectSegments[i];
        applyArcanRedirect(ascr, wnd, ascr->redirectSegments[i].C, i);
        trace("findFreeSegment(%d) reuse %d", (int)wnd->drawable.id, (int)i);
        break;
    }

/* synchronously allocate the window - this avoids a possible extra copy and
 * pixmap / exposure juggling versus doing it asynchronously, but with the
 * blocking downsides. These should subside after an initial burst (unless it
 * keeps allocating until the peak, like during a DoS/fork bomb, which the
 * Arcan side should manage anyhow */
    if (best || !blockAlloc || (0 == __builtin_ffsll(~ascr->redirectBitmap)))
       return best;

    int bw = wnd->borderWidth;

    ARCAN_ENQUEUE(ascr->acon, &(struct arcan_event){
           .ext.kind = ARCAN_EVENT(SEGREQ),
           .ext.segreq.width = wnd->drawable.width + bw + bw,
           .ext.segreq.height = wnd->drawable.height + bw + bw,
           .ext.segreq.kind = SEGID_BRIDGE_X11,
           .ext.segreq.id = wnd->drawable.id
    });

/* block and maintain the normal event loop on primary segment until we get
 * a newsegment or a reqfail for the new segment */
    arcan_event ev;
    while (arcan_shmif_wait(ascr->acon, &ev)){
        int status = arcanEventDispatch(ascr->acon, ascr, &ev, wnd->drawable.id);
        if (status == -2){
           trace("findFreeSegment() parent rejected window allocation");
           break;
        }
        else if (status == 2){
            trace("findFreeSegment() new subsegment acquired");
            struct arcan_shmif_cont *C = malloc(sizeof(struct arcan_shmif_cont));
            if (!C)
                return NULL;
            *C = arcan_shmif_acquire(ascr->acon, NULL, SEGID_BRIDGE_X11, SHMIF_DISABLE_GUARD);
            if (!C->addr){
                free(C);
                return NULL;
            }
            best = redirectWindow(ascr, wnd, C);
            break;
       }
    }

    return best;
}

static Bool arcanRealizeWindow(WindowPtr wnd)
{
    Bool rv = true;
    arcanScrPriv *ascr = getArcanScreen(wnd);
    trace("realizeWindow(%d)", (int) wnd->drawable.id);

/* First let the rest of the infrastructure deal with realize, it is somewhat
 * unclear if this is better to do AFTER we've established our own patches to
 * the structure or not. */
    if (ascr->hooks.realizeWindow){
        ascr->screen->RealizeWindow = ascr->hooks.realizeWindow;
        rv = ascr->hooks.realizeWindow(wnd);
        ascr->screen->RealizeWindow = arcanRealizeWindow;
    }

/* Mark the window and its children as having its state invalidated, if the
 * arcan end wants metadata to be synched this means that during the next block
 * it will push the VIEWPORT event set of the subgraph */
    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE),
    };
    snprintf(
             (char*)ev.ext.message.data, 78,
             "kind=realize:xid=%d",
             (int) wnd->drawable.id
    );
    ARCAN_ENQUEUE(ascr->acon, &ev);
    flagUnsynched(wnd);

    if (!ascr->defaultRootless)
        return rv;

/* Might need special treatment / tracking if the window is an overlay */
    if (!wnd->parent){
        BoxRec box = {0, 0, ascr->acon->w, ascr->acon->h};
        RegionReset(&wnd->winSize, &box);
        RegionNull(&wnd->clipList);
        RegionNull(&wnd->borderClip);
    }

    if (!isRedirectCandidate(ascr, wnd))
        return rv;

/* See if there already is a segment allocated that we can re-use in order to
 * trade allocation budget for latency - ideally it should also be fit to size
 * in order to avoid the reallocation handshake. That, in turn, might need a
 * worker thread. */
    arcanWndPriv* apriv = malloc(sizeof(arcanWndPriv));
    *apriv = (arcanWndPriv){
        .redirect = findFreeSegment(ascr, wnd, true)
    };

/* block the window from drawing into the pixmap of its parent, we will copy
 * that ourselves, then do the rest when the allocation has gone through. */
    compRedirectWindow(serverClient, wnd, CompositeRedirectManual);
    compRedirectSubwindows(serverClient, wnd, CompositeRedirectAutomatic);

    dixSetPrivate(&wnd->devPrivates, &windowPriv, apriv);
    return rv;
}

static Bool arcanUnrealizeWindow(WindowPtr wnd)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    arcanWndPriv *awnd = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    trace("UnrealizeWindow(%d)", (int) wnd->drawable.id);
    Bool rv = true;

/* Remove any active redirection - hide or drop the shmif context depending
 * on if it is likely to get re-used (defaultRootless) or not. If it is, mark
 * it available for re-use. */
    if (awnd && awnd->redirect){
/*      compUnredirectWindow(serverClient, wnd, CompositeRedirectManual); */
        awnd->redirect->redirected = false;
        if (awnd->redirect->C){
            if (awnd->redirect->damage){
                DamageUnregister(awnd->redirect->damage);
                awnd->redirect->damage = NULL;
            }
            if (!ascr->defaultRootless){
                arcan_shmif_drop(awnd->redirect->C);
                awnd->redirect->C = NULL;
            }
            else {
                arcan_shmif_enqueue(
                                   awnd->redirect->C,
                                   &(struct arcan_event){
                                     .category = EVENT_EXTERNAL,
                                     .ext.kind = ARCAN_EVENT(VIEWPORT),
                                     .ext.viewport.invisible = true
                                  });
           }
        }
    }

    if (ascr->hooks.unrealizeWindow){
        ascr->screen->UnrealizeWindow = ascr->hooks.unrealizeWindow;
        rv = ascr->hooks.unrealizeWindow(wnd);
        ascr->screen->UnrealizeWindow = arcanUnrealizeWindow;
    }

    if (ascr->wmSynch){
        struct arcan_event ev = (struct arcan_event){
            .ext.kind = ARCAN_EVENT(MESSAGE),
        };
        snprintf(
                 (char*)ev.ext.message.data, 78,
                 "kind=unrealize:xid=%d",
                 (int) wnd->drawable.id
        );
        ARCAN_ENQUEUE(ascr->acon, &ev);
    }

    if (awnd)
        free(awnd);
    dixSetPrivate(&wnd->devPrivates, &windowPriv, NULL);
    return rv;
}

static void arcanRestackWindow(WindowPtr wnd, WindowPtr oldNextSibling)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    trace("RestackWindow(%d)", (int) wnd->drawable.id);

    if (ascr->hooks.restackWindow){
        ascr->screen->RestackWindow = ascr->hooks.restackWindow;
        ascr->hooks.restackWindow(wnd, oldNextSibling);
        ascr->screen->RestackWindow = arcanRestackWindow;
    }

    if (!ascr->wmSynch)
        return;

    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE),
    };
    snprintf(
             (char*)ev.ext.message.data, 78,
             "kind=restack:xid=%d:parent=%d:next=%d%s",
             (int) wnd->drawable.id,
             wnd->parent ? (int) wnd->parent->drawable.id : -1,
             wnd->nextSib ? (int) wnd->nextSib->drawable.id : -1,
             wnd->prevSib ? ":first" : ""
            );
    ARCAN_ENQUEUE(ascr->acon, &ev);
}

static Bool arcanCreateWindow(WindowPtr wnd)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    Bool res = true;
    trace("CreateWindow(%d)", (int) wnd->drawable.id);
    ascr->windowCount++;

    if (ascr->hooks.createWindow){
        ascr->screen->CreateWindow = ascr->hooks.createWindow;
        res = ascr->hooks.createWindow(wnd);
        ascr->screen->CreateWindow = arcanCreateWindow;
    }

    if (!ascr->wmSynch)
        return res;

    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE),
    };
    snprintf(
             (char*)ev.ext.message.data, 78,
             "kind=create:xid=%d:parent=%d:next=%d",
             (int) wnd->drawable.id,
             wnd->parent ? (int) wnd->parent->drawable.id : -1,
             wnd->nextSib ? (int) wnd->nextSib->drawable.id : -1
    );
    ARCAN_ENQUEUE(ascr->acon, &ev);
    return res;
}

static Bool arcanDestroyWindow(WindowPtr wnd)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    trace("DestroyWindow(%d)", (int) wnd->drawable.id);
    Bool res = true;
    ascr->windowCount--;

    if (!ascr->wmSynch)
        return res;

/* this has no clean mapping, hack around it with a message */
    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE)
    };
    snprintf((char*)ev.ext.message.data, 78,
             "kind=destroy:xid=%d", (int)wnd->drawable.id);
    ARCAN_ENQUEUE(ascr->acon, &ev);

    if (ascr->hooks.destroyWindow){
        ascr->screen->DestroyWindow = ascr->hooks.destroyWindow;
        res = ascr->hooks.destroyWindow(wnd);
        ascr->screen->DestroyWindow = arcanDestroyWindow;
    }

    return res;
}

static int arcanConfigureWindow(WindowPtr wnd, int x, int y, int w, int h, int bw, WindowPtr sibling)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    arcanWndPriv *awnd = dixLookupPrivate(&wnd->devPrivates, &windowPriv);

    trace("ConfigureWindow(%d, %d, %d, %d) - %d", x, y, w, h, sibling ? (int) sibling->drawable.id : -1);
    int res = 0;

    if (ascr->hooks.configureWindow){
        ascr->screen->ConfigNotify = ascr->hooks.configureWindow;
        res = ascr->hooks.configureWindow(wnd, x, y, w, h, bw, sibling);
        ascr->screen->ConfigNotify = arcanConfigureWindow;
    }

/* synch the mapping between our dst-copy pixmap and the configured w/h and leave room for the border */
    bw += bw;
    if (awnd && awnd->redirect){
        struct arcan_shmif_cont *C = awnd->redirect->C;

        if (w + bw != C->w || h + bw != C->h){
            arcan_shmif_resize(C, w + bw, h + bw);
            ascr->screen->ModifyPixmapHeader(awnd->redirect->pmap,
                                             C->w, C->h,
                                             24, BitsPerPixel(24),
                                             C->stride,
                                             C->vidp);
        }
    }

    flagUnsynched(wnd);
    return res;
}

static void
arcanResizeWindow(WindowPtr wnd, int x, int y, unsigned w, unsigned h, WindowPtr sibling)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    trace("ResizeWindow(%d, %d, %d, %d) - %d", x, y, w, h, sibling ? (int) sibling->drawable.id : -1);
    if (ascr->hooks.resizeWindow){
        ascr->screen->ResizeWindow = ascr->hooks.resizeWindow;
        ascr->hooks.resizeWindow(wnd, x, y, w, h, sibling);
        ascr->screen->ResizeWindow = arcanResizeWindow;
    }

/* note: changeWindowAttributes tell us (SubstructureRedirectMask | ResizeRedirectMask)
 *       if the client is a window manager or not, and if the direct parent of a window
 *       is the window manager, or we have manually marked the window as having its own
 *       segment
 */
     if (ht_find(ascr->proxyMap, &wnd->drawable.id)){
         synchTreeDepth(ascr, ascr->screen->root, sendWndData, true, ascr->acon);
    }
    else
        flagUnsynched(wnd);
}

Bool
arcanScreenInitialize(KdScreenInfo * screen, arcanScrPriv * scrpriv)
{
    scrpriv->acon->hints =
        SHMIF_RHINT_SUBREGION    |
        SHMIF_RHINT_IGNORE_ALPHA |
        SHMIF_RHINT_VSIGNAL_EV;
    scrpriv->dirty = true;
    trace("arcanScreenInitialize");

    if (!screen->width || !screen->height) {
        screen->width = scrpriv->acon->w;
        screen->height = scrpriv->acon->h;
    }

/* if the size dimensions exceed the permitted values, shrink. Also try and
 * enable the color control subprotocol */
    if (!arcan_shmif_resize_ext(scrpriv->acon, screen->width, screen->height,
        (struct shmif_resize_ext){
#ifdef RANDR
        .meta = SHMIF_META_CM
#endif
    })){
            screen->width = scrpriv->acon->w;
            screen->height = scrpriv->acon->h;
    }

    arcan_shmifsub_getramp(scrpriv->acon, 0, &scrpriv->block);

/* default guess, we cache this between screen init/deinit */
    if (!arcan_init)
        arcan_shmif_initial(scrpriv->acon, &arcan_init);

    if (arcan_init->density > 0){
        screen->width_mm = (float)screen->width / (0.1 * arcan_init->density);
        screen->height_mm = (float)screen->height / (0.1 * arcan_init->density);
    }

    scrpriv->randr = screen->randr;
    if (arcanGlamor)
        setGlamorMask(screen);
    else
        setArcanMask(screen);

    return arcanMapFramebuffer(screen);
}

static int convertMouseButton(int btn)
{
    int ind = 0;
    if (btn == 0 || btn > 10)
        return -1;

    switch (btn){
        case MBTN_LEFT_IND: return 1; break;
        case MBTN_RIGHT_IND: return 3; break;
        case MBTN_MIDDLE_IND: return 2; break;
        case MBTN_WHEEL_UP_IND: return 4; break;
        case MBTN_WHEEL_DOWN_IND: return 5; break;
        default:
            return ind + 8;
        break;
    }
}

static void
TranslateInput(struct arcan_shmif_cont* con, arcan_event* oev)
{
    int x = 0, y = 0;
    arcan_ioevent *ev = &oev->io;
    if (ev->devkind == EVENT_IDEVKIND_MOUSE){
/*
 * There's a trick here -
 *
 * The problems with FPS and other games that wholly or partially grab+warp to try and get
 * relative mouse input is that the WM end in Arcan gets to implement the warp.
 * This is inherently racey, as multiple input samples can happen before the next warp.
 *
 * With relative mouse samples (gotrel) [0] and [2] will be dx, dy and [1, 3] calculated
 * context- absolute positions and for absolute samples (gotrel=false), [0, 2] will be absolute
 * and [1, 3] relative. For both cases we can compare to the last sent 'warp' position as the
 * equality of absx+rx, absy+ry should match up with warpx-absx, warpy-absy or we get the
 * error to compensate for.
 */
        if (ev->datatype == EVENT_IDATATYPE_ANALOG){
            if (ev->input.analog.gotrel){
                ValuatorMask mask;
                int valuators[3] = {0, 0, 0};
                if (ev->subid == 2){
                    valuators[0] = ev->input.analog.axisval[0];
                    valuators[1] = ev->input.analog.axisval[2];
                }
                else
                    valuators[ev->input.analog.axisval[ev->subid % 2]] = ev->input.analog.axisval[0];
                valuator_mask_set_range(&mask, 0, 3, valuators);
                QueuePointerEvents(arcanInputPriv.pi->dixdev, MotionNotify, 0, POINTER_RELATIVE, &mask);
            }
            else if (arcan_shmif_mousestate(con, arcanInputPriv.mstate, oev, &x, &y)){
                ValuatorMask mask;
                int valuators[3] = {x, y, 0};
                valuator_mask_set_range(&mask, 0, 3, valuators);
                QueuePointerEvents(arcanInputPriv.pi->dixdev,
                                   MotionNotify,
                                   0, /* buttons must be 0 for motion */
                                   POINTER_DESKTOP | POINTER_ABSOLUTE,
                                   &mask
                                   );
            }
        }
        else {
              int index = convertMouseButton(ev->subid);
              if (index < 0)
                  return;

              if (ev->input.digital.active)
                  mouseButtonBitmap |= 1 << index;
              else
                  mouseButtonBitmap &= ~(1 << index);
              ValuatorMask mask;
              valuator_mask_zero(&mask);
              QueuePointerEvents(arcanInputPriv.pi->dixdev,
                                 ev->input.digital.active ? ButtonPress : ButtonRelease,
                                 index,
                                 0,
                                 &mask
                                 );
        }
    }
    else if (ev->datatype == EVENT_IDATATYPE_TRANSLATED){
        code_tbl[ev->input.translated.scancode % 512] = ev->input.translated.active;
        enqueueKeyboard(
            ev->input.translated.scancode, ev->input.translated.active);
  }
}

#ifdef RANDR
static Bool
arcanRandRScreenResize(ScreenPtr pScreen,
    CARD16 width, CARD16 height, CARD32 mmWidth, CARD32 mmHeight);
#else
static Bool
arcanRandRScreenResize(ScreenPtr pScreen,
    CARD16 width, CARD16 height, CARD32 mmWidth, CARD32 mmHeight)
{
    return false;
}
#endif

static
void
arcanDisplayHint(struct arcan_shmif_cont* con,
                 int w, int h, int fl, int rgb, float ppcm)
{
    arcanScrPriv* apriv = con->user;

/* release on focus loss, open point, does this actually cover mods? */
    if (fl & 4){
        for (size_t i = 0; i < sizeof(code_tbl) / sizeof(code_tbl[0]); i++){
            if (code_tbl[i]){
                trace("force-release:%d", code_tbl[i]);
                enqueueKeyboard(i, 1);
                code_tbl[i] = 0;
            }
        }
        KdEnqueuePointerEvent(arcanInputPriv.pi, KD_MOUSE_DELTA, 0, 0, 0);
    }

    if (arcanConfigPriv.no_dynamic_resize)
        return;

    if (w >= 640 && h >= 480 && (con->w != w || con->h != h) && !(fl & 1)){
#ifdef RANDR
       if (ppcm == 0 && arcan_init)
           ppcm = arcan_init->density;

       RRScreenSetSizeRange(apriv->screen,
                            640, 480, PP_SHMPAGE_MAXW, PP_SHMPAGE_MAXH);
       arcanRandRScreenResize(apriv->screen,
                              w, h,
                              ppcm > 0 ? (float)w / (0.1 * ppcm) : 0,
                              ppcm > 0 ? (float)h / (0.1 * ppcm) : 0
                             );
        RRScreenSizeNotify(apriv->screen);
        return;
#endif
    }
}

static
void
synchTreeDepth(arcanScrPriv* scrpriv, WindowPtr wnd,
      void (*callback)(WindowPtr node, int depth, int max_depth, void* tag),
         bool force,
         void* tag)
{
    int depth = 0;

    while(wnd){
        if (wnd->unsynched || force)
            callback(wnd, depth, scrpriv->windowCount, tag);
        if (wnd->firstChild){
            wnd = wnd->firstChild;
            depth++;
            continue;
        }
        while (wnd && !wnd->nextSib){
            wnd = wnd->parent;
            depth--;
        }
        if (!wnd)
            break;
        wnd = wnd->nextSib;
    }
}

static
void
arcanSignal(struct arcan_shmif_cont* con, bool dirty)
{
    arcanScrPriv *scrpriv = con->user;
    scrpriv->dirty |= dirty;

    if (!scrpriv->dirty)
        return;

    scrpriv->unsynched = 0;

/* To avoid a storm of event -> update, don't signal or attempt to update
 * before the segment has finished synchronising the last one. */
    if (arcan_shmif_signalstatus(con))
        return;

     if (scrpriv->wmSynch)
         synchTreeDepth(scrpriv, scrpriv->screen->root, sendWndData, false, con);

    RegionPtr region;
    bool in_glamor = false;

    region = DamageRegion(scrpriv->damage);
    updateDirty(scrpriv->acon, scrpriv->damage);
    arcanSynchCursor(scrpriv, false);

    if (!RegionNotEmpty(region) || scrpriv->defaultRootless)
        return;

/*
 * We don't use fine-grained dirty regions really, the data gathered gave
 * quite few benefits as cases with many dirty regions quickly exceeded the
 * magic ratio where subtex- update vs full texture update tipped in favor
 * of the latter.
 */
#ifdef GLAMOR
    in_glamor = scrpriv->in_glamor;
    if (in_glamor){
        int fd = -1;
        size_t stride = 0;
        int fourcc = 0;

        if (scrpriv->bo){
            fd = gbm_bo_get_fd(scrpriv->bo);
            stride = gbm_bo_get_stride(scrpriv->bo);
            fourcc = DRM_FORMAT_XRGB8888;
        }
        else if (scrpriv->tex != -1){
            uintptr_t disp;
            arcan_shmifext_egl_meta(scrpriv->acon, &disp, NULL, NULL);
            arcan_shmifext_gltex_handle(scrpriv->acon, disp,
                                        scrpriv->tex, &fd, &stride, &fourcc);
        }

        arcan_shmif_signalhandle(scrpriv->acon,
                                 SHMIF_SIGVID | SHMIF_SIGBLK_NONE,
                                 fd, stride, fourcc);

        if (-1 != scrpriv->pending_fd){
            close(scrpriv->pending_fd);
        }
        scrpriv->pending_fd = fd;
    }
#endif

/* The other bit is that we might want to be able to toggle to front-buffer
 * only scanout, the mechanism for that is through DEVICE_NODE providing a
 * different vbuffer, we swap the signalling buffer and then rely on the
 * VSIGNAL to carry. The dirty region is reset on signal. */
    if (!in_glamor)
        arcan_shmif_signal(con, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);

    DamageEmpty(scrpriv->damage);
    scrpriv->dirty = false;
}

static
void
cmdCreateProxyWindow(
                       struct arcan_shmif_cont *con,
                       int x, int y, int w, int h, unsigned long vid)
{
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, AF_UNIX, pair);

    struct proxyWindowData *proxy = malloc(sizeof(struct proxyWindowData));
    *proxy = (struct proxyWindowData){
        .socket = pair[1],
        .w = w,
        .h = h,
        .x = x,
        .y = y,
        .arcan_vid = vid,
        .cont = con
    };

    pthread_attr_t pthattr;
    pthread_attr_init(&pthattr);
    pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);

    AddClientOnOpenFD(pair[0]);
    pthread_t pth;
    pthread_create(&pth, &pthattr, (void*)(void*)arcanProxyWindowDispatch, proxy);
}

static
void
cmdSpawnBuiltinWM(void)
{
    if (!arcanConfigPriv.miniwm && !arcanConfigPriv.wmexec)
        return;

    if (arcanConfigPriv.miniwm)
    {
        int pair[2];
        socketpair(AF_UNIX, SOCK_STREAM, AF_UNIX, pair);

        struct proxyWindowData *proxy = malloc(sizeof(struct proxyWindowData));
        *proxy = (struct proxyWindowData){
            .socket = pair[1]
     };

        pthread_attr_t pthattr;
        pthread_attr_init(&pthattr);
        pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);

        AddClientOnOpenFD(pair[0]);

        pthread_t pth;
        pthread_create(&pth, &pthattr, (void*)(void*)arcanMiniWMDispatch, proxy);
        return;
    }

/* This one is quite annoying as neither xcb nor xlib permits is to specify
 * a process-inherited descriptor to the socket which would really be the
 * proper thing to do. */
    pid_t pid = fork();
    if (-1 == pid){
        ErrorF("couldn't spawn wm child\n");
        return;
    }

    if (0 == pid){
        char* const argv[] = {(char*)arcanConfigPriv.wmexec, NULL};
        char buf[16];
        snprintf(buf, 16, ":%s", display);
        setenv("DISPLAY", buf, 1);
        unsetenv("ARCAN_CONNPATH");
        execvp(arcanConfigPriv.wmexec, argv);
        exit(EXIT_FAILURE);
    }
}

static
void
cmdClipboardWindow(struct arcan_shmif_cont *con, bool paste)
{
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, AF_UNIX, pair);

    struct proxyWindowData *proxy = malloc(sizeof(struct proxyWindowData));
    *proxy = (struct proxyWindowData){
        .socket = pair[1],
        .cont = con,
        .paste = paste
    };

    pthread_attr_t pthattr;
    pthread_attr_init(&pthattr);
    pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);

    AddClientOnOpenFD(pair[0]);
    pthread_t pth;
    pthread_create(&pth, &pthattr, (void*)(void*)arcanClipboardDispatch, proxy);
}

static
void
getSizePos(struct arg_arr *cmd,
           int32_t *x, int32_t *y,
           uint32_t *w, uint32_t *h,
           bool* got_xy)
{
    const char* tmp;
    *got_xy = false;
    *x = *y = *w = *h = 0;

    if (arg_lookup(cmd, "x", 0, &tmp) && tmp){
        *x = (int32_t) strtol(tmp, NULL, 10);
    }

    if (arg_lookup(cmd, "y", 0, &tmp) && tmp){
        *y = (int32_t) strtol(tmp, NULL, 10);
    }

    if (arg_lookup(cmd, "w", 0, &tmp) && tmp){
        *w = (uint32_t) strtoul(tmp, NULL, 10);
        *got_xy = true;
    }

    if (arg_lookup(cmd, "h", 0, &tmp) && tmp){
        *h = (uint32_t) strtoul(tmp, NULL, 10);
        *got_xy = true;
    }
}

static
int dumpTreeMeta(WindowPtr window, void *data)
{
    FILE* fout = data;

    fprintf(fout, "%u\[label=\"%u\" shape=%s]\n",
      window->drawable.id,
      window->drawable.id,
      isWindowVisible(window) ? "circle" : "square"
    );
/* open question which attributes makes sense to present here and as what,
 * important states are - drawable, not drawable, redirected - masks,
 * damaged, descendants damaged, 'foreign' (e.g. arcan)
 * - */
    return WT_WALKCHILDREN;
}

static
int dumpTreeRelations(WindowPtr window, void *data)
{
    FILE* fout = data;

    WindowPtr child = window->firstChild;
    while (child){
        fprintf(fout, "\"%u\"->\"%d\";\n",
                      window->drawable.id,
                      child->drawable.id);
        child = child->nextSib;
    }
    return WT_WALKCHILDREN;
}

static void wndMetaToSVGFile(WindowPtr wnd, int depth, int max_depth, void* tag)
{
    uint8_t intens = 64 + (float)(depth+1) / (float)(max_depth+1) * 127.0;

/* this assumes that depth works incrementally, hence the synchTree call that
 * works in breadth-first. */
    fprintf((FILE*)tag,
            "<rect stroke=\"%s\" fill-opacity=\"%d%%\" id=\"wnd_%u\" "
            "width=\"%d\" height=\"%d\" x=\"%d\" y=\"%d\" fill=\"#%02X%02X%02X\"/>\n",
            isWindowVisible(wnd) ? "#00ff00" : "#ff0000",
            isWindowVisible(wnd) ? 100 : 20,
            wnd->drawable.id, wnd->drawable.width, wnd->drawable.height,
            wnd->drawable.x, wnd->drawable.y, intens, intens, intens
           );
}

static void
dumpTree(arcanScrPriv *ascr, int fd, const char* message)
{
    FILE* fout = fdopen(fd, "w");
    if (!fout){
        close(fd);
        return;
    }

/* for the graph processed view we use BFS and present the rectangles in our
 * intended composition order */
    if (strcasecmp(message, "svg") == 0){
        fprintf(fout,
                      "<svg width=\"%zu\" height=\"%zu\""
                      "xmlns=\"http://www.w3.org/2000/svg\"><g>\n",
                      ascr->acon->w, ascr->acon->h
              );
        synchTreeDepth(ascr, ascr->screen->root, wndMetaToSVGFile, true, fout);
        fprintf(fout, "</g></svg>\n");
        return;
    }

/* two pass - first one adds each window, second plots out the relationships */
    fprintf(fout, "digraph g{\n");
      TraverseTree(ascr->screen->root, dumpTreeMeta, fout);
      TraverseTree(ascr->screen->root, dumpTreeRelations, fout);
    fprintf(fout, "}\n");
    fclose(fout);
}

static void
cmdReconfigureWindow(int32_t x, int32_t y, uint32_t w, uint32_t h, bool xy, XID id)
{
    WindowPtr res;
    if (Success != dixLookupResourceByType((void**) &res,
                                           id, RT_WINDOW,
                                           NULL, DixWriteAccess)){
        return;
    }

    ClientPtr client;
    if (Success != dixLookupClient(&client, id, NULL, DixWriteAccess)){
        return;
    }

/* Fake a message to ourselves, seems like there never was a real internal
 * way of moving/configuring a window - should perhaps add that instead of
 * these kinds of hacks. */
    if (xy){
        XID vlist[4] = {x, y, w, h};
        ConfigureWindow(res, CWX | CWY | CWWidth | CWHeight, vlist, client);
    }
    else{
        XID vlist[2] = {w, h};
        ConfigureWindow(res, CWWidth | CWHeight, vlist, client);
     }
}

static void
cmdRedirectWindow(struct arcan_shmif_cont *con, XID id)
{
    WindowPtr res;
    if (Success != dixLookupResourceByType((void**) &res, id, RT_WINDOW, NULL, DixWriteAccess))
        return;

    arcan_shmif_enqueue(con, &(struct arcan_event){
            .ext.kind = ARCAN_EVENT(SEGREQ),
            .ext.segreq.width = res->drawable.width,
            .ext.segreq.height = res->drawable.height,
            .ext.segreq.kind = SEGID_BRIDGE_X11,
            .ext.segreq.id = id
        });
}

static
void
decodeMessage(struct arcan_shmif_cont* con, const char* msg)
{
    struct arg_arr* cmd = arg_unpack(msg);
    if (!cmd){
        trace("error in command: %s", msg);
        return;
    }

    const char* strid = NULL;
    int64_t id = -1;
    if (arg_lookup(cmd, "id", 0, &strid) && strid){
        id = strtol(strid, NULL, 10);
    }

    const char* kind = NULL;
    if (!arg_lookup(cmd, "kind", 0, &kind) || !kind)
        goto cleanup;

    bool newwnd = strcmp(kind, "new") == 0;
    bool modwnd = strcmp(kind, "configure") == 0;
    bool redirwnd = strcmp(kind, "redirect") == 0;

    if (redirwnd){
        cmdRedirectWindow(con, id);
        return;
    }

/* it's noisy so only enable on demand */
    if (strcmp(kind, "synch") == 0){
        arcanScrPriv* ascr= con->user;
        ascr->wmSynch = true;
        synchTreeDepth(con->user, ascr->screen->root, resetWndData, true, con);
        return;
    }

    if ((newwnd || modwnd) && id >= 0){
        int32_t x, y;
        uint32_t w, h;
        bool got_xy;

        getSizePos(cmd, &x, &y, &w, &h, &got_xy);
        if (!w || !h)
            goto cleanup;

        if (newwnd)
            cmdCreateProxyWindow(con, x, y, w, h, id);
        else
            cmdReconfigureWindow(x, y, w, h, id, got_xy);
    }
    else if (strcmp(kind, "destroy") == 0 && id >= 0){
        FreeResource(id, RT_NONE);
    }

cleanup:
    arg_cleanup(cmd);
}

static void
damageReport(DamagePtr damage, RegionPtr region, void *tag) {
/* forward the damage rect to the shmif, with SUBREGION_CHAIN (if it gets
 * implemented) we can signal here already and not think about the blocking,
 * otherwise just mark now and let the present or block-handler trigger */
    struct redirectMeta *rm = tag;
    updateDirty(rm->C, damage);
    rm->dirty = true;
}

static void
damageDestroy(DamagePtr damage, void *tag)
{
}

static PixmapPtr
pixmapFromShmif(ScreenPtr pScreen, struct arcan_shmif_cont *C, unsigned usage)
{
    PixmapPtr pPixmap = AllocatePixmap(pScreen, 0);
    if (!pPixmap)
        return pPixmap;

    pPixmap->drawable.type = DRAWABLE_PIXMAP;
    pPixmap->drawable.class = 0;
    pPixmap->drawable.pScreen = pScreen;
    pPixmap->drawable.bitsPerPixel = 24;
    pPixmap->drawable.id = 0;
    pPixmap->drawable.serialNumber = NEXT_SERIAL_NUMBER;
    pPixmap->drawable.x = 0;
    pPixmap->drawable.y = 0;
    pPixmap->drawable.width = C->w;
    pPixmap->drawable.height = C->h;
    pPixmap->refcnt = 1;
    pPixmap->devPrivate.ptr = NULL;
    pPixmap->primary_pixmap = NULL;
    pPixmap->screen_x = 0;
    pPixmap->screen_y = 0;
    pPixmap->usage_hint = usage;
    pScreen->ModifyPixmapHeader(pPixmap, C->w, C->h, 24, BitsPerPixel(24), C->stride, C->vidp);
    return pPixmap;
/* mark it in our static list, walk it and set user for it */
}

static void applyArcanRedirect(arcanScrPriv *S,
                               WindowPtr wnd, struct arcan_shmif_cont *C, uint64_t i)
{
/* RHINT_IGNORE_ALPHA, CSPACE_SRGB, ORIGO_UL */
    C->hints = SHMIF_RHINT_SUBREGION | SHMIF_RHINT_CSPACE_SRGB | SHMIF_RHINT_IGNORE_ALPHA;
    arcan_shmif_resize(C, wnd->drawable.width, wnd->drawable.height);
    PixmapPtr pmap = pixmapFromShmif(wnd->drawable.pScreen, C, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

   C->user = wnd;

/* Attach a damage tracker that will track dirty updates and rectangles used
 * for the blit from the redirected backing pixmap to the context one. It
 * should be possible to compSetPixmap the shm segment directly and save a full
 * copy(!) but there seem to be quite some patching needed to make sure the x/y
 * positions of the window adjust into the buffer when the GC is working on it.
 */
    C->dirty = (struct arcan_shmif_region){0};
    S->redirectBitmap |= (uint64_t)1 << i;
    struct redirectMeta *rm = &S->redirectSegments[i];
    rm->C = C;
    rm->redirected = true;
    rm->pmap = pmap;
    rm->damage = DamageCreate(
                              damageReport,
                              damageDestroy,
                              DamageReportNonEmpty,
                              FALSE, S->screen, rm
                             );
    DamageRegister(&wnd->drawable, rm->damage);
    DamageSetReportAfterOp(rm->damage, true);
    trace("redirectWindow(%d:%d)", wnd->drawable.id, (int) i);

/* FIX: If we are running with a regular window manager in hybrid mode, marking
 * the redirected surface as override redirected to (hopefully) allow the WM to
 * ignore it.
 *
 * Attach a default handler event loop that tracks events on it as part of the
 * block handler, mainly to make sure focus events and so-on gets translated
 * correctly.
 */
}

static void
handleNewSegment(struct arcan_shmif_cont *C, arcanScrPriv *S, int kind, bool out, unsigned id)
{
    if (out){
/* This should be bound to a pixmap and replace the one in ID if it matches a
 * specific drawable. The pixmap itself should never be resized. If the ID is unset,
 * a new proxy window should be created with this as its X local backing.
 *
 * If the ID matches (-1) it should instead be used to substitute the root when
 * someone is trying to read or copy from it.
 */
    }
    else if (kind == SEGID_CLIPBOARD || kind == SEGID_CLIPBOARD_PASTE){
        struct arcan_shmif_cont *clip = malloc(sizeof(struct arcan_shmif_cont));
        *clip = arcan_shmif_acquire(C, NULL, kind, SHMIF_DISABLE_GUARD);
        cmdClipboardWindow(clip, kind == SEGID_CLIPBOARD_PASTE);
    }
    else if (kind == SEGID_CURSOR){
        if (S->cursor){
            arcan_shmif_drop(S->cursor);
            S->cursor = NULL;
        }
        S->cursor = malloc(sizeof(struct arcan_shmif_cont));
        if (S->cursor){
            *(S->cursor) = arcan_shmif_acquire(C, NULL, SEGID_CURSOR, SHMIF_DISABLE_GUARD);
        }
    }
    else if (id){
        WindowPtr wnd;
        if (Success != dixLookupResourceByType((void**) &wnd, id, RT_WINDOW, NULL, DixWriteAccess)){
            trace("redirect on unknown ID");
            return;
        }
        struct arcan_shmif_cont *pxout = malloc(sizeof(struct arcan_shmif_cont));
        *pxout = arcan_shmif_acquire(C, NULL, kind, SHMIF_DISABLE_GUARD);
        redirectWindow(S, wnd, pxout);
 /* if we just redirect a single window into a separate shmif buffer, Auto is fine to keep the
 * screen pixmap intact (though could defer that based on displayhint visibility) */
        compRedirectWindow(serverClient, wnd, CompositeRedirectManual);
        compRedirectSubwindows(serverClient, wnd, CompositeRedirectAutomatic);
    }
}

/*
 * Process the primary segment event loop. This can be called from the normal
 * multiplexed event handler that Xorg has, OR as a way of waiting for a
 * response to a query of a new subsegment (see defaultRedirect). In the later
 * case the pairing ID is set in [uid] and a REQFAIL or NEWSEGMENT on uid will
 * return rather than keep processing. Otherwise the handleNewSegment routine
 * is used.
 */
int
arcanEventDispatch(struct arcan_shmif_cont* con, arcanScrPriv* ascr, arcan_event* aev, int64_t uid)
{
    struct arcan_event ev = *aev;
    if (ev.category == EVENT_IO){
        TranslateInput(con, &ev);
        return 0;
    }
    else if (ev.category != EVENT_TARGET)
        return 0;

    switch (ev.tgt.kind){
    case TARGET_COMMAND_STEPFRAME:
        return 1;
    break;
    case TARGET_COMMAND_MESSAGE:
        decodeMessage(con, ev.tgt.message);
    break;
    case TARGET_COMMAND_RESET:
        switch(ev.tgt.ioevs[0].iv){
/* not much 'state' that we can extract or track from the Xorg internals,
 * possibly input related and likely not worth it */
        case 0:
        case 1:
        case 2:
        break;
/* swap-out monitored FD */
        case 3:
            InputThreadUnregisterDev(con->epipe);
            InputThreadRegisterDev(con->epipe, (void*) arcanFlushEvents, con);
            ascr->dirty = 1;
            return 1;
        break;
        }
/* all the window metadata need to be resent as create, and the ones that
 * are paired to a vid needs to have the corresponding pair being sent */
        if (ascr->wmSynch){
            synchTreeDepth(con->user, ascr->screen->root, resetWndData, true, con);
        }
        return 1;
        break;
    case TARGET_COMMAND_DISPLAYHINT:
        arcanDisplayHint(con,
                         ev.tgt.ioevs[0].iv, ev.tgt.ioevs[1].iv,
                         ev.tgt.ioevs[2].iv, ev.tgt.ioevs[3].iv, ev.tgt.ioevs[4].iv);
    break;
    case TARGET_COMMAND_OUTPUTHINT:
    break;
    case TARGET_COMMAND_BCHUNK_OUT:
        dumpTree(ascr,
                 arcan_shmif_dupfd(ev.tgt.ioevs[0].iv, -1, false),
                 ev.tgt.message
                );
    break;
    case TARGET_COMMAND_REQFAIL:
        return -2;
    break;
    case TARGET_COMMAND_NEWSEGMENT:
        if (uid > 0 && ev.tgt.ioevs[3].uiv == uid){
            return 2;
        }
        else
            handleNewSegment(con,
                             con->user,
                             ev.tgt.ioevs[2].iv,
                             ev.tgt.ioevs[1].iv,
                             ev.tgt.ioevs[3].uiv);
    break;
    case TARGET_COMMAND_EXIT:
        input_unlock();
        return -1;
    break;
    default:
    break;
    }

    return 0;
}

void
arcanFlushEvents(int fd, void* tag)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    if (!con || !con->user)
        return;

    input_lock();
    arcanScrPriv* ascr = con->user;
    arcan_event ev;
    int rv, status = 0;

    while ((rv = arcan_shmif_poll(con, &ev)) > 0){
        status = arcanEventDispatch(con, ascr, &ev, -1);
    }

    if (rv < 0)
        status = -1;

/* in the redirected mode we don't need to synch the main buffer ever and any
 * subsegment synch is performed as part of the dirty management / present on
 * that segment - just make sure the cursor is still updated */
    if (ascr->defaultRootless){
        arcanSynchCursor(ascr, false);
     }
    else if (status == 1)
      arcanSignal(con, false);

    input_unlock();

/* This should only happen if crash recovery fail and the segment is dead. The
 * bizarre question is what to do in the event of different screens connected
 * to different display servers with one having been redirected to networked */
    if (-1 == status){
        CloseWellKnownConnections();
        OsCleanup(1);
        dispatchException |= DE_TERMINATE;
    }
}

Bool
arcanScreenInit(KdScreenInfo * screen)
{
    arcanScrPriv *scrpriv;
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    trace("arcanScreenInit");
    if (!con)
        return FALSE;

    scrpriv = calloc(1, sizeof(arcanScrPriv));
    if (!scrpriv)
        return FALSE;

    if (!dixRegisterPrivateKey(&pixmapPriv, PRIVATE_PIXMAP, 0)){
        return FALSE;
    }

    if (!dixRegisterPrivateKey(&windowPriv, PRIVATE_WINDOW, 0)){
        return FALSE;
    }

/* primary connection is a allocated once and then retained for the length of
 * the process */
    if (con->user){
        fprintf(stderr, "multiple screen support still missing\n");
        abort();
    }

    scrpriv->proxyMap = ht_create(
        sizeof(XID),
        sizeof(uintptr_t), ht_resourceid_hash, ht_resourceid_compare, NULL
    );

    scrpriv->pending_fd = -1;
    scrpriv->acon = con;
    arcan_shmifsub_getramp(scrpriv->acon, 0, &scrpriv->block);
    con->user = scrpriv;

    if (!scrpriv->acon){
        free(scrpriv);
        return FALSE;
    }

    screen->driver = scrpriv;
    if (!arcanScreenInitialize(screen, scrpriv)) {
        screen->driver = 0;
        free(scrpriv);
        return FALSE;
    }

    return TRUE;
}

static void arcanInternalDamageRedisplay(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    if (!pScreen)
        return;

/* The segment might already be in a locked state, if so the next STEPFRAME
 * event will wake us up and synch, otherwise send it right away. */
    scrpriv->dirty = true;
    arcanSignal(scrpriv->acon, false);
}

static void onDamageDestroy(DamagePtr damage, void *closure)
{
    trace("OnArcanDamageDestroy(%p)", damage);
}

static Bool arcanSetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pPixmap = NULL;

    scrpriv->damage = DamageCreate((DamageReportFunc) 0,
                                   (DamageDestroyFunc) onDamageDestroy,
                                   DamageReportBoundingBox,
                                   TRUE, pScreen, pScreen);

    trace("arcanSetInternalDamage(%p)", scrpriv->damage);
    pPixmap = (*pScreen->GetScreenPixmap) (pScreen);

    DamageRegister(&pPixmap->drawable, scrpriv->damage);
    DamageSetReportAfterOp(scrpriv->damage, TRUE);

    return TRUE;
}

Bool
arcanCreateResources(ScreenPtr pScreen)
{
    trace("arcanCreateResources");
    return arcanSetInternalDamage(pScreen);
}

static void arcanUnsetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("arcanUnsetInternalDamage(%p)", scrpriv->damage);
    if (scrpriv->damage){
        DamageUnregister(scrpriv->damage);
        DamageDestroy(scrpriv->damage);
        scrpriv->damage = NULL;
    }
}

static int depth_to_gbm(int depth)
{
    if (depth == 16)
        return GBM_FORMAT_RGB565;
    else if (depth == 24)
        return GBM_FORMAT_XRGB8888;
    else if (depth == 30)
        return GBM_FORMAT_ARGB2101010;
    else if (depth != 32)
        ErrorF("unhandled gbm depth: %d\n", depth);

    return GBM_FORMAT_ARGB8888;
}

static
int ArcanSetPixmapVisitWindow(WindowPtr window, void *data)
{
    ScreenPtr screen = window->drawable.pScreen;
    trace("arcanSetPixmapVisitWindow");
    if (screen->GetWindowPixmap(window) == data){
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }
    return WT_DONTWALKCHILDREN;
}

#ifdef GLAMOR

static
PixmapPtr boToPixmap(ScreenPtr pScreen, struct gbm_bo* bo, int depth)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pixmap;
    struct pixmap_ext *ext_pixmap;
    uintptr_t adisp, actx;
 /* Unfortunately shmifext- don't expose the buffer-import setup yet,
 * waiting for the whole GBM v Streams to sort itself out, so just
 * replicate that code once more. */
/* helper_dmabuf_eglimage(agp_fenv, agp_eglenv, EGLDisplay, planes, n_planes) */
    pixmap = glamor_create_pixmap(pScreen,
                                  gbm_bo_get_width(bo),
                                  gbm_bo_get_height(bo),
                                  depth,
                                  GLAMOR_CREATE_PIXMAP_NO_TEXTURE);

    if (pixmap == NULL) {
        trace("ArcanDRI3PixmapFromFD()::Couldn't create pixmap from BO");
        gbm_bo_destroy(bo);
        return NULL;
    }

    arcan_shmifext_make_current(scrpriv->acon);
    ext_pixmap = malloc(sizeof(struct pixmap_ext));
    if (!ext_pixmap){
        trace("ArcanDRI3PixmapFromFD()::Couldn't allocate pixmap metadata");
        gbm_bo_destroy(bo);
        glamor_destroy_pixmap(pixmap);
        return NULL;
    }

    *ext_pixmap = (struct pixmap_ext){0};
    arcan_shmifext_egl_meta(scrpriv->acon, &adisp, NULL, &actx);
    ext_pixmap->bo = bo;
    ext_pixmap->image = eglCreateImageKHR((EGLDisplay) adisp,
                                          (EGLContext) actx,
                                          EGL_NATIVE_PIXMAP_KHR,
                                          ext_pixmap->bo, NULL);

        if (ext_pixmap->image == EGL_NO_IMAGE_KHR){
            trace("ArcanDRI3PixmapFromFD()::eglImageFromBO failed");
            free(ext_pixmap);
            glamor_destroy_pixmap(pixmap);
            return NULL;
        }

    glGenTextures(1, &ext_pixmap->texture);
    glBindTexture(GL_TEXTURE_2D, ext_pixmap->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ext_pixmap->image);
        if (eglGetError() != EGL_SUCCESS){
            trace("ArcanDRI3PixmapFromFD()::eglImageToTexture failed");
            glamor_destroy_pixmap(pixmap);
            eglDestroyImageKHR((EGLDisplay) adisp, ext_pixmap->image);
            free(ext_pixmap);
            return NULL;
        }

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!glamor_set_pixmap_texture(pixmap, ext_pixmap->texture)){
            trace("ArcanDRI3PixmapFromFD()::glamor rejected pixmap<->texture");
            glamor_destroy_pixmap(pixmap);
            eglDestroyImageKHR((EGLDisplay) adisp, ext_pixmap->image);
            free(ext_pixmap);
            return NULL;
        }

    dixSetPrivate(&pixmap->devPrivates, &pixmapPriv, ext_pixmap);
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    return pixmap;
}
#endif

static
Bool arcanGlamorCreateScreenResources(ScreenPtr pScreen)
{
#ifdef GLAMOR
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr oldpix, newpix = NULL;
    uintptr_t dev;
    int fmt;

    trace("arcanGlamorCreateScreenResources");

/* get the allocation device reference */
    oldpix = pScreen->GetScreenPixmap(pScreen);
    if (-1 == arcan_shmifext_dev(scrpriv->acon, &dev, false)){
        trace("ArcanDRI3PixmapFromFD()::Couldn't get device handle");
        return false;
    }

/* get the format for the buffer and convert to a pixmap based on the
 * screen output depth format */
    fmt = depth_to_gbm(pScreen->rootDepth);
    if (-1 != fmt){
        struct gbm_bo *bo = gbm_bo_create((struct gbm_device*) dev,
                                          pScreen->width,
                                          pScreen->height,
                                          fmt,
                                          GBM_BO_USE_SCANOUT |
                                          GBM_BO_USE_RENDERING);
        newpix = boToPixmap(pScreen, bo, pScreen->rootDepth);
        scrpriv->bo = bo;
    }

/*  if there's no device native object created, build a separate intermediate
 *  texture (or failed dest) bound to a pixmap */
    if (!newpix){
        newpix = pScreen->CreatePixmap(pScreen,
                                       pScreen->width,
                                       pScreen->height,
                                       pScreen->rootDepth,
                                       CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
        scrpriv->bo = NULL;
        scrpriv->tex = (newpix ? glamor_get_pixmap_texture(newpix) : -1);
    }

/* now walk all windows and swap out the backing store for this pixmap */
    if (newpix){
        trace("SetScreenPixmap(new)");
        pScreen->SetScreenPixmap(newpix);
/*      glamor_set_screen_pixmap(newpix, NULL); */

      if (pScreen->root && pScreen->SetWindowPixmap)
            TraverseTree(pScreen->root, ArcanSetPixmapVisitWindow, oldpix);

        SetRootClip(pScreen, ROOT_CLIP_FULL);
    }

    return TRUE;
#endif
   return FALSE;
}

Bool
arcanMapFramebuffer(KdScreenInfo * screen)
{
    arcanScrPriv *scrpriv = screen->driver;
    KdPointerMatrix m;

    trace("arcanMapFramebuffer");
    KdComputePointerMatrix(&m, scrpriv->randr, screen->width, screen->height);
    KdSetPointerMatrix(&m);

    if (scrpriv->in_glamor){
        screen->fb.frameBuffer = NULL;
    }
    else {
        screen->fb.byteStride = scrpriv->acon->stride;
        screen->fb.pixelStride = scrpriv->acon->pitch;
        screen->fb.frameBuffer = (CARD8 *) (scrpriv->acon->vidp);
    }
    return TRUE;
}

void
arcanSetScreenSizes(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    float ind = ARCAN_SHMPAGE_DEFAULT_PPCM * 0.1;
    int inw, inh;

/* default guess */
    trace("arcanSetScreenSizes");
    inw = scrpriv->acon->w;
    inh = scrpriv->acon->h;

    if (arcan_init && arcan_init->density > 0)
        ind = arcan_init->density * 0.1;

    pScreen->width = inw;
    pScreen->height = inh;
    pScreen->mmWidth = (float) inw / ind;
    pScreen->mmHeight = (float) inh / ind;
}

Bool
arcanUnmapFramebuffer(KdScreenInfo * screen)
{
    trace("arcanUnmapFramebuffer");
/*
 * free(priv->base);
    priv->base = NULL;
 */
    return TRUE;
}

static void
redirectSignalDirty(struct redirectMeta *M)
{
/* If we do this manually, re-extract the extents from ->C and GC copy, also
 * resize. Two big optimizations is to move this to a worker thread, to let the
 * backing store be handled by XComposite and ofc. dri- buffers */
    WindowPtr pWnd = M->C->user;
    arcanWndPriv *awnd = dixLookupPrivate(&pWnd->devPrivates, &windowPriv);
    arcanScrPriv* ascr = getArcanScreen(pWnd);
    struct arcan_shmif_region dirty = M->C->dirty;
    int x = dirty.x1;
    int y = dirty.y1;
    int h = dirty.y2 - y;
    int w = dirty.x2 - x;

    if (w < 0 || h < 0 || (!w && !h))
        return;

    CompWindowPtr cw = GetCompWindow(pWnd);
    PixmapPtr pmap = (pWnd->drawable.pScreen)->GetWindowPixmap(pWnd);

    if (cw && cw->pOldPixmap){
        pmap = cw->pOldPixmap;
    }

    sendWndData(pWnd, 0, 0, M->C);
    copyDrawableShmif(ascr, M, pmap, dirty);
    DamageEmpty(awnd->redirect->damage);
    arcan_shmif_signal(M->C, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

int arcanInit(void)
{
    struct arcan_shmif_cont* con = calloc(1, sizeof(struct arcan_shmif_cont));
    char dispstr[512] = "";
    if (!con)
        return 0;

/* With -wmexec we may need sparse allocation of X11 displays, and
 * CreateWellKnownSockets will be called after OsInit. If displayfd is notset
 * and no display has been explicity set, it will exit if the 0 display is
 * already taken. By setting displayfd here we force the sparse allocation path
 * to be taken. */
        if (-1 == displayfd){
            displayfd = open("/dev/null", O_WRONLY);
        }

/* windisplay.c */
    trace("ArcanInit");
    if (_XSERVTransIsListening("local")){
        snprintf(dispstr, 512, "%s:%d", display, 0);
    }
    else if (_XSERVTransIsListening("inet")){
        snprintf(dispstr, 512, "127.0.0.1:%s.%d", display, 0);
    }
    else if (_XSERVTransIsListening("inet6")){
        snprintf(dispstr, 512, "::1:%s.%d", display, 0);
    }

    *con = arcan_shmif_open_ext(0, NULL, (struct shmif_open_ext){
        .type = SEGID_BRIDGE_X11,
        arcanConfigPriv.title ? arcanConfigPriv.title : "Xorg",
        arcanConfigPriv.ident ? arcanConfigPriv.ident : dispstr
    }, sizeof(struct shmif_open_ext));
    if (!con->addr){
        free(con);
        return 0;
    }

/* indicate that we can/want to do color-management, RandR will check this
 * with the arcan_shmif_substruct calls */
    arcan_shmif_resize_ext(con, con->w, con->h,
        (struct shmif_resize_ext){
#ifdef RANDR
        .meta = SHMIF_META_CM
#endif
    });

/* request a cursor and a clipboard, if they arrive we initialize that code
 * as well, otherwise we have our own implementation of save-under + cursor
 * bitblit */
    if (!arcanConfigPriv.soft_mouse)
        arcan_shmif_enqueue(con, &(struct arcan_event){
            .ext.kind = ARCAN_EVENT(SEGREQ),
            .ext.segreq.width = 32,
            .ext.segreq.height = 32,
            .ext.segreq.kind = SEGID_CURSOR,
            .ext.segreq.id = CURSOR_REQUEST_ID
        });
    else
        arcan_shmif_enqueue(con, &(arcan_event){
            .ext.kind = ARCAN_EVENT(CURSORHINT),
            .ext.message.data = "hidden"
        });

    arcan_shmif_enqueue(con, &(struct arcan_event){
        .ext.kind = ARCAN_EVENT(SEGREQ),
        .ext.segreq.width = 32,
        .ext.segreq.height = 32,
        .ext.segreq.kind = SEGID_CLIPBOARD,
        .ext.segreq.id = CLIPBOARD_REQUEST_ID
    });

/* announce a 'dot' and 'svg' exported file extensions for getting a snapshot
 * of the window tree in order to make debugging much less painful */
    arcan_shmif_enqueue(con, &(struct arcan_event){
        .ext.kind = ARCAN_EVENT(BCHUNKSTATE),
        .category = EVENT_EXTERNAL,
        .ext.bchunk = {
                       .input = false,
                       .hint  = 0,
                       .extensions = "dot;svg"
                      }
    });

    arcan_shmif_setprimary(SHMIF_INPUT, con);

    return 1;
}

void arcanFini(void)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    trace("ArcanFini");
    if (con){
        arcan_shmif_drop(con);
    }
}

#ifdef RANDR
Bool
arcanRandRGetInfo(ScreenPtr pScreen, Rotation * rotations)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    RRScreenSizePtr pSize;
    Rotation randr;
    int n;

    trace("ArcanRandRGetInfo");
    *rotations = RR_Rotate_0;

    for (n = 0; n < pScreen->numDepths; n++)
        if (pScreen->allowedDepths[n].numVids)
            break;
    if (n == pScreen->numDepths)
        return FALSE;

    pSize = RRRegisterSize(pScreen,
                           screen->width,
                           screen->height, screen->width_mm, screen->height_mm);

    randr = KdSubRotation(scrpriv->randr, screen->randr);

    RRSetCurrentConfig(pScreen, randr, 0, pSize);

    return TRUE;
}

static Bool
arcanRandRScreenResize(ScreenPtr pScreen,
    CARD16 width, CARD16 height, CARD32 mmWidth, CARD32 mmHeight)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    RRScreenSize size = {0};
    arcanScrPriv *scrpriv = screen->driver;

    if (width == screen->width && height == screen->height)
        return FALSE;

    trace("ArcanRandRScreenResize");
    size.width = width;
    size.height = height;
    size.mmWidth = mmWidth;
    size.mmHeight = mmHeight;

    if (arcanRandRSetConfig(pScreen, screen->randr, 0, &size)){
            RRModePtr mode = arcan_cvt(width, height, (float) arcan_init->rate / 1000.0, 0, 0);

        if (!scrpriv->randrOutput){
          trace("arcanRandRInit(No output)");
            return FALSE;
        }

/* need something else here, like Crtcnotify - bigger mode table than 'just the one'
 * is probably needed for picky games where / if that still matters -- RRCrtcNotify
 * seems to be a broadcast 'this has changed' for everyone (which might be useful
 * for a dedicated XRandr like path) */
        RROutputSetModes(scrpriv->randrOutput, &mode, 1, 1);
        RRCrtcGammaSetSize(scrpriv->randrCrtc, scrpriv->block.plane_sizes[0] / 3);
        RROutputSetPhysicalSize(scrpriv->randrOutput, size.mmWidth, size.mmHeight);

/*
 RCrtcNotify(scrpriv->randrOutput, mode, 0, 0, RR_ROTATE_0, NULL, 1, ???)
 */
   }

    return TRUE;
}

Bool
arcanRandRSetConfig(ScreenPtr pScreen,
                   Rotation randr, int rate, RRScreenSizePtr pSize)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    Bool wasEnabled = pScreenPriv->enabled;
    arcanScrPriv oldscr;
    int oldwidth;
    int oldheight;
    int oldmmwidth;
    int oldmmheight;
    int newwidth, newheight;

/* Ignore rotations etc. If those are desired properties, it will happen on
 * a higher level. */
    trace("ArcanRandRSetConfig");
    newwidth = pSize->width;
    newheight = pSize->height;

    if (wasEnabled)
        KdDisableScreen(pScreen);

    oldscr = *scrpriv;

    oldwidth = screen->width;
    oldheight = screen->height;
    oldmmwidth = pScreen->mmWidth;
    oldmmheight = pScreen->mmHeight;

    /*
     * Set new configuration
     */

    scrpriv->randr = KdAddRotation(screen->randr, randr);
    arcan_shmif_resize_ext(scrpriv->acon, newwidth, newheight,
        (struct shmif_resize_ext){
#ifdef RANDR
        .meta = SHMIF_META_CM
#endif
        });
    arcan_shmifsub_getramp(scrpriv->acon, 0, &scrpriv->block);
    arcanUnmapFramebuffer(screen);

    if (!arcanMapFramebuffer(screen))
        goto bail4;

    arcanUnsetInternalDamage(pScreen);

    arcanSetScreenSizes(screen->pScreen);

#ifdef GLAMOR
    if (arcanConfigPriv.glamor){
        arcan_shmifext_make_current(scrpriv->acon);
        arcanGlamorCreateScreenResources(pScreen);
    }
#endif
    if (!arcanSetInternalDamage(screen->pScreen))
        goto bail4;

    /*
     * Set frame buffer mapping
     */
    (*pScreen->ModifyPixmapHeader) (fbGetScreenPixmap(pScreen),
                                    pScreen->width,
                                    pScreen->height,
                                    screen->fb.depth,
                                    screen->fb.bitsPerPixel,
                                    screen->fb.byteStride,
                                    screen->fb.frameBuffer);

    KdSetSubpixelOrder(pScreen, scrpriv->randr);

    if (wasEnabled)
        KdEnableScreen(pScreen);

    RRScreenSizeNotify(pScreen);

    return TRUE;

 bail4:
    arcanUnmapFramebuffer(screen);
    *scrpriv = oldscr;
    (void) arcanMapFramebuffer(screen);
    pScreen->width = oldwidth;
    pScreen->height = oldheight;
    pScreen->mmWidth = oldmmwidth;
    pScreen->mmHeight = oldmmheight;

    if (wasEnabled)
        KdEnableScreen(pScreen);
    return FALSE;
}

#if RANDR_12_INTERFACE
static Bool arcanRandRSetGamma(ScreenPtr pScreen, RRCrtcPtr crtc)
{
    KdScreenPriv(pScreen);
/*   rrScrPrivPtr pScrPriv; */
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    size_t plane_sz = scrpriv->block.plane_sizes[0] / 3;

    for (size_t i = 0, j = 0; i < crtc->gammaSize && j < plane_sz; i++, j++){
        scrpriv->block.planes[i] = (float)crtc->gammaRed[i] / 65536.0f;
        scrpriv->block.planes[i+plane_sz] = (float)crtc->gammaGreen[i] / 65536.0f;
        scrpriv->block.planes[i+2*plane_sz] = (float)crtc->gammaBlue[i] / 65536.0f;
    }

    return arcan_shmifsub_setramp(scrpriv->acon, 0, &scrpriv->block) ? TRUE : FALSE;
}

static Bool arcanRandRGetGamma(ScreenPtr pScreen, RRCrtcPtr crtc)
{
    KdScreenPriv(pScreen);
/*  rrScrPrivPtr pScrPriv; */
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    arcan_shmifsub_getramp(scrpriv->acon, 0, &scrpriv->block);
    size_t plane_sz = scrpriv->block.plane_sizes[0] / 3;

    for (size_t i = 0; i < crtc->gammaSize && i < scrpriv->block.plane_sizes[0]; i++){
        crtc->gammaRed[i] = scrpriv->block.planes[i] * 65535;
        crtc->gammaGreen[i] = scrpriv->block.planes[i + plane_sz] * 65535;
        crtc->gammaBlue[i] = scrpriv->block.planes[i + plane_sz + plane_sz] * 65535;
    }

    return TRUE;
}
#endif

Bool
arcanRandRInit(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    rrScrPrivPtr pScrPriv;
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    scrpriv->screen = pScreen;

    if (!RRScreenInit(pScreen))
        return FALSE;

    trace("ArcanRandRInit");
    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = arcanRandRGetInfo;
    pScrPriv->rrSetConfig = arcanRandRSetConfig;

    RRScreenSetSizeRange(pScreen, 640, 480, PP_SHMPAGE_MAXW, PP_SHMPAGE_MAXH);

#if RANDR_12_INTERFACE
    pScrPriv->rrScreenSetSize = arcanRandRScreenResize;
    pScrPriv->rrCrtcSetGamma = arcanRandRSetGamma;
    pScrPriv->rrCrtcGetGamma = arcanRandRGetGamma;
#endif

/* NOTE regarding data model:
 * to the RandR screen, we create an Output that is assigned a Crtc
 * to which we attach one or several modes. We then notify rander as to
 * changes in the Crtc config. It is the gamma ramps of this crtc that
 * we can then finally access. */
    scrpriv->randrOutput = RROutputCreate(pScreen, "screen", 6, NULL);
    scrpriv->randrCrtc = RRCrtcCreate(pScreen, scrpriv->randrOutput);
    if (!scrpriv->randrCrtc){
        trace("arcanRandRInit(Failed to create CRTC)");
        return FALSE;
    }

    RRCrtcSetRotations(scrpriv->randrCrtc, RR_Rotate_0);
    RRCrtcGammaSetSize(scrpriv->randrCrtc, scrpriv->block.plane_sizes[0] / 3);

    RROutputSetCrtcs(scrpriv->randrOutput, &scrpriv->randrCrtc, 1);
    RROutputSetConnection(scrpriv->randrOutput, RR_Connected);

    return TRUE;
}
#endif

#ifdef GLAMOR
static
void arcanGlamorEglMakeCurrent(struct glamor_context *ctx)
{
    trace("ArcanGlamorEglMakeCurrent");
    arcan_shmifext_make_current((struct arcan_shmif_cont*) ctx->ctx);
}

_X_EXPORT int
glamor_egl_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                          CARD16 *stride, CARD32 *size)
{
    return -1;
}

_X_EXPORT int
glamor_egl_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *strides, uint32_t *offsets,
                           uint64_t *modifier)
{
    return 0;
/* return number of fds, write into fds, trides, offsets, modifiers */
}

/*
 * int glamor_egl_dri3_fd_name_from_tex(ScreenPtr pScreen,
                                     PixmapPtr pixmap,
                                     unsigned int tex,
                                     Bool want_name, CARD16 *stride, CARD32 *size)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    size_t dstr;
    int fmt, fd;

    trace("(arcan) glamor_egl_dri3_fd_name_from_tex");
    *size = pixmap->drawable.width * (*stride);

    if (arcan_shmifext_gltex_handle(scrpriv->acon, 0,
                                    tex, &fd, &dstr, &fmt)){
        *stride = dstr;
        return fd;
    }

    *stride = 0;
    *size = 0;
    return -1;
}
 */

void
glamor_egl_screen_init(ScreenPtr pScreen, struct glamor_context *glamor_ctx)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    uintptr_t egl_disp = 0, egl_ctx = 0;
    arcan_shmifext_egl_meta(scrpriv->acon, &egl_disp, NULL, &egl_ctx);

    trace("(Arcan) glamor_egl_screen_init");
    glamor_ctx->ctx = (void*)(scrpriv->acon);
    glamor_ctx->display = (EGLDisplay) egl_disp;
    glamor_ctx->make_current = arcanGlamorEglMakeCurrent;
    arcan_shmifext_make_current(scrpriv->acon);
/*
 * something when this is done breaks damage regions
 */
    if (!arcanConfigPriv.no_dri3)
        glamor_enable_dri3(pScreen);
}

void arcanGlamorEnable(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("ArcanGlamorEnable");
    scrpriv->in_glamor = TRUE;
 }

void arcanGlamorDisable(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("ArcanGlamorDisable");
    if (scrpriv){
        scrpriv->in_glamor = FALSE;
    }
 }

void arcanGlamorFini(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("ArcanGlamorFini");
    if (scrpriv){
        arcan_shmifext_drop(scrpriv->acon);
    }
    glamor_fini(pScreen);
}

static int dri3FdFromPixmap(ScreenPtr pScreen, PixmapPtr pixmap,
                            CARD16 *stride, CARD32 *size)
{
    struct pixmap_ext *ext_pixmap = dixLookupPrivate(
                                             &pixmap->devPrivates,
                                             &pixmapPriv);
    trace("ArcanDRI3FdFromPixmap");
    if (!ext_pixmap || !ext_pixmap->bo){
        return -1;
    }

    *stride = gbm_bo_get_stride(ext_pixmap->bo);
    *size = pixmap->drawable.width * *stride;

    return gbm_bo_get_fd(ext_pixmap->bo);
}

static PixmapPtr dri3PixmapFromFds(ScreenPtr pScreen,
                       CARD8 num_fds, const int *fds,
                       CARD16 width, CARD16 height,
                       const CARD32 *strides, const CARD32 *offsets,
                       CARD8 depth, CARD8 bpp, uint64_t modifier)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    struct gbm_device *gbmdev;
    struct gbm_bo *bo = NULL;

    if (!scrpriv->in_glamor){
        trace("ArcanDRI3PixmapFromFD()::Not in GLAMOR state");
        return NULL;
    }

/* reject invalid values outright */
    if (!width || !height || !num_fds || depth < 16 ||
         bpp != BitsPerPixel(depth) || strides[0] < (width * bpp / 8)){
        trace("ArcanDRI3PixmapFromFD::invalid input arguments");
        return NULL;
    }

    uintptr_t dev_handle;
    if (-1 == arcan_shmifext_dev(scrpriv->acon, &dev_handle, false)){
        trace("ArcanDRI3PixmapFromFD()::Couldn't get device handle");
        return NULL;
    }
    else
        gbmdev = (struct gbm_device*) dev_handle;

    int gbm_fmt = depth_to_gbm(depth);
    if (modifier != DRM_FORMAT_MOD_INVALID){
        struct gbm_import_fd_modifier_data data = {
            .width = width,
            .height = height,
            .num_fds = num_fds,
            .format = gbm_fmt,
            .modifier = modifier
        };
        for (size_t i = 0; i < num_fds; i++){
            data.fds[i] = fds[i];
            data.strides[i] = strides[i];
            data.offsets[i] = offsets[i];
        }
        bo = gbm_bo_import(gbmdev,
                           GBM_BO_IMPORT_FD_MODIFIER,
                           &data,
                           GBM_BO_USE_RENDERING);
    }
    else if (num_fds == 1){
        struct gbm_import_fd_data data = {
            .fd = fds[0],
            .width = width,
            .height = height,
            .stride = strides[0],
            .format = gbm_fmt
        };

        bo = gbm_bo_import(gbmdev, GBM_BO_IMPORT_FD, &data, GBM_BO_USE_RENDERING);
        if (!bo){
            trace("ArcanDRI3PixmapFromFD()::BO_mod_invalid import fail");
            return NULL;
        }
    }

    return boToPixmap(pScreen, bo, depth);
}

static int dri3Open(ClientPtr client,
                    ScreenPtr pScreen,
                    RRProviderPtr provider,
                    int *pfd)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    int fd;

    if (arcanConfigPriv.no_dri3)
        return BadAlloc;

    fd = arcan_shmifext_dev(scrpriv->acon, NULL, true);

    trace("ArcanDri3Open(%d)", fd);
    if (-1 != fd){
        *pfd = fd;
        return Success;
    }

    return BadAlloc;
}

static Bool dri3GetFormats(ScreenPtr screen,
                           CARD32 *num_formats, CARD32 **formats)
{
    trace("dri3GetFormats");
    *num_formats = 0;
    return FALSE;
}

static Bool dri3GetDrawableModifiers(DrawablePtr screen,
                                     uint32_t format,
                                     uint32_t *num_modifiers, uint64_t **modifiers)
{
    trace("dri3GetDrawableModifiers");
    num_modifiers = 0;
    return FALSE;
}

static Bool dri3GetModifiers(ScreenPtr screen,
                             uint32_t format,
                             uint32_t *num_modifiers, uint64_t **modifiers)
{
    trace("dri3GetModifiers");
    *num_modifiers = 0;
    return FALSE;
}

static dri3_screen_info_rec dri3_info = {
    .version = 2,
    .open_client = dri3Open,
    .pixmap_from_fds = dri3PixmapFromFds,
    .fd_from_pixmap = dri3FdFromPixmap,
    .get_formats = dri3GetFormats,
    .get_modifiers = dri3GetModifiers,
    .get_drawable_modifiers = dri3GetDrawableModifiers
};

Bool arcanGlamorInit(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

/* It may be better to create a subsurface that we escalate to GL, and map
 * the placement of this surface relative to the display similar to old-style
 * overlays. */
    struct arcan_shmifext_setup defs = arcan_shmifext_defaults(scrpriv->acon);
    int errc;
    defs.depth = 0;
    defs.alpha = 0;
    defs.major = GLAMOR_GL_CORE_VER_MAJOR;
    defs.minor = GLAMOR_GL_CORE_VER_MINOR;
    defs.mask  = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
    trace("arcanGlamorInit");

    defs.builtin_fbo = false;
    errc = arcan_shmifext_setup(scrpriv->acon, defs);
    if (errc != SHMIFEXT_OK){
        ErrorF("xarcan/glamor::init() - EGL context failed (reason: %d), "
               "lowering version\n", errc);
        defs.major = 2;
        defs.minor = 1;
        defs.mask = 0;
        if (SHMIFEXT_OK != arcan_shmifext_setup(scrpriv->acon, defs)){
            ErrorF("xarcan/glamor::init(21) - EGL context failed again, giving up");
            return FALSE;
        }
    }
#ifdef XV
/*   Seem to be some Kdrive wrapper to deal with less of this crap,
 *   though is it still relevant or even / needed?
 *   arcanGlamorXvInit(pScreen);
 */
#endif

    if (glamor_init(pScreen, GLAMOR_USE_EGL_SCREEN)){
        scrpriv->in_glamor = TRUE;
    }
    else {
        ErrorF("arcanGlamorInit() - failed to initialize glamor");
        goto bail;
    }
    if (!arcanConfigPriv.no_dri3){
        if (!dri3_screen_init(pScreen, &dri3_info)){
            ErrorF("arcanGlamorInit() - failed to set DRI3");
            arcan_shmifext_drop(scrpriv->acon);
            goto bail;
        }
    }

    scrpriv->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = arcanGlamorCreateScreenResources;

    return TRUE;
bail:
    arcan_shmifext_drop(scrpriv->acon);
    scrpriv->in_glamor = FALSE;
    return FALSE;
}
#endif

static void
arcanSetWindowPixmap(WindowPtr wnd, PixmapPtr pixmap)
{
    arcanScrPriv* ascr = getArcanScreen(wnd);
    if (ascr->hooks.setWindowPixmap){
        ascr->screen->SetWindowPixmap = ascr->hooks.setWindowPixmap;
        ascr->hooks.setWindowPixmap(wnd, pixmap);
        ascr->screen->SetWindowPixmap = arcanSetWindowPixmap;
    }
}

static void
arcanScreenBlockHandler(ScreenPtr pScreen, void* timeout)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    pScreen->BlockHandler = scrpriv->BlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

    if (scrpriv->redirectBitmap)
    {
        for (size_t i = 0; i < 64; i++){
            if (!scrpriv->redirectSegments[i].redirected)
                continue;

            redirectSignalDirty(&scrpriv->redirectSegments[i]);
        }
    }
    scrpriv->dirtyBitmap = 0;

    if (scrpriv->damage)
        arcanInternalDamageRedisplay(pScreen);
/*
 * else if (!arcan_shmif_signalstatus)
        arcan_shmif_signal(scrpriv->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
 */
}

Bool
arcanCreateColormap(ColormapPtr pmap)
{
    trace("arcanCreateColormap");
    return fbInitializeColormap(pmap);
}

Bool
arcanInitScreen(ScreenPtr pScreen)
{
    pScreen->CreateColormap = arcanCreateColormap;
    trace("arcanInitScreen");
    return TRUE;
}

static Bool
arcanCloseScreenWrap(ScreenPtr pScreen)
{
    trace("arcanCloseScreenWrap");
/*  arcanCloseScreen(pScreen); */
    return TRUE;
}
static RRCrtcPtr
arcanPresentGetCrtc(WindowPtr window)
{
    ScreenPtr pScreen = window->drawable.pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("present:get_crtc");

    if (!scrpriv)
        return NULL;

    return scrpriv->randrCrtc;
}

static int arcanPresentGetUstMsc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    trace("present:get_ust_msc");
    return 0;
}

static void arcanPresentAbortVblank(
                                    RRCrtcPtr crtc,
                                    uint64_t evid,
                                    uint64_t msc)
{
    trace("present:vblank abort:fix");
    /* if there is a pending evid for vblank, it should be removed */
}

static int arcanPresentQueueVblank(RRCrtcPtr crtc, uint64_t evid, uint64_t msc)
{
/* evid should be fired via present_event_notify when crtc has reached msc, for
 * the composited form msc matches with vpts received from shmif after a signal,
 * which doesn't strictly match what a possible output would be on, but could if
 * it is mapped directly rather than nested composition. */
    trace("present:queue vblank (wait for vready)");
    return Success;
}

static void
arcanPresentFlush(WindowPtr window)
{
    trace("present:flush");
#ifdef GLAMOR
    ScreenPtr pScreen = window->drawable.pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    if (scrpriv->in_glamor)
        glamor_block_handler(window->drawable.pScreen);
#endif
}

static Bool
arcanPresentFlip(RRCrtcPtr crtc,
                 uint64_t event_id,
                 uint64_t target_msc,
                 PixmapPtr pixmap,
                 Bool sync_flip)
{
/* if sync flip is true, wait for "blank" then signal */
    trace("present:flip");
    return true;
}

static void
arcanPresentUnflip(ScreenPtr screen, uint64_t eventid)
{
    trace("present:unflip");
}

/* can 'window' be flipped - for locally composited, this should only apply if
 * the window is fullscreen (e.g. no visible sprites, ...) if the window is
 * redirected it can be flipped unless one is pending and synch flip is set,
 * and always if sync_flip isn't set. That could be used to cap buffer in
 * flight depth. */
static Bool
arcanPresentCheckFlip(RRCrtcPtr crtc,
                      WindowPtr window,
                      PixmapPtr pixmap,
                      Bool sync_flip) /* Flip2 has a reason for failure */
{
    trace("present:check flip");
    arcanWndPriv *awnd = dixLookupPrivate(&window->devPrivates, &windowPriv);
    if (awnd && awnd->redirect){
        return true;
    }
    return false;
}

/* there are more interesting callbacks in present_priv,
 * present_pixmap for one as that one exposes wait_fence and idle_fence */
static present_screen_info_rec arcan_present_info = {
    .version      = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc     = arcanPresentGetCrtc,
    .get_ust_msc  = arcanPresentGetUstMsc,
    .queue_vblank = arcanPresentQueueVblank,
    .abort_vblank = arcanPresentAbortVblank,
    .flush        = arcanPresentFlush,
    .capabilities = PresentCapabilityAsync,
#ifdef GLAMOR
    .check_flip   = arcanPresentCheckFlip,
    .flip         = arcanPresentFlip,
    .unflip       = arcanPresentUnflip
#endif
};

Bool
arcanFinishInitScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    trace("arcanFinishInitScreen");
#ifdef RANDR
    if (!arcanRandRInit(pScreen))
        return FALSE;
#endif

    scrpriv->CloseHandler = pScreen->CloseScreen;
    pScreen->CloseScreen = arcanCloseScreenWrap;
    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

    if (arcanConfigPriv.present)
        present_screen_init(pScreen, &arcan_present_info);

/* hook window actions so that we can map / forward - disable to troubleshoot */
#ifndef BLOCK_HOOKS
    scrpriv->hooks.positionWindow = screen->pScreen->PositionWindow;
    scrpriv->hooks.changeWindow = screen->pScreen->ChangeWindowAttributes;
    scrpriv->hooks.realizeWindow = screen->pScreen->RealizeWindow;
    scrpriv->hooks.unrealizeWindow = screen->pScreen->UnrealizeWindow;
    scrpriv->hooks.restackWindow = screen->pScreen->RestackWindow;
    scrpriv->hooks.destroyWindow = screen->pScreen->DestroyWindow;
    scrpriv->hooks.getImage = screen->pScreen->GetImage;
    scrpriv->hooks.configureWindow = screen->pScreen->ConfigNotify;
    scrpriv->hooks.resizeWindow = screen->pScreen->ResizeWindow;
    scrpriv->hooks.createWindow = screen->pScreen->CreateWindow;
    scrpriv->hooks.markOverlappedWindows = screen->pScreen->MarkOverlappedWindows;
    scrpriv->hooks.setWindowPixmap = screen->pScreen->SetWindowPixmap;
    scrpriv->hooks.getWindowPixmap = screen->pScreen->GetWindowPixmap;
    scrpriv->hooks.interposeDrawableSrcDst = screen->pScreen->InterposeDrawableSrcDst;

    screen->pScreen->PositionWindow = arcanPositionWindow;
    screen->pScreen->GetImage = arcanGetImage;
    screen->pScreen->GetWindowPixmap = arcanGetWindowPixmap;
    screen->pScreen->RealizeWindow = arcanRealizeWindow;
    screen->pScreen->UnrealizeWindow = arcanUnrealizeWindow;
    screen->pScreen->RestackWindow = arcanRestackWindow;
    screen->pScreen->DestroyWindow = arcanDestroyWindow;
    screen->pScreen->ConfigNotify = arcanConfigureWindow;
    screen->pScreen->ResizeWindow = arcanResizeWindow;
    screen->pScreen->CreateWindow = arcanCreateWindow;
    screen->pScreen->MarkOverlappedWindows = arcanMarkOverlapped;
    screen->pScreen->SetWindowPixmap = arcanSetWindowPixmap;
    screen->pScreen->InterposeDrawableSrcDst = arcanDrawableInterposeSrcDst;
#endif

    arcan_shmif_mousestate_setup(scrpriv->acon, false, arcanInputPriv.mstate);
    if (arcanConfigPriv.redirect)
        scrpriv->defaultRootless = true;

    cmdSpawnBuiltinWM();
    return TRUE;
}

void
arcanScreenFini(KdScreenInfo * screen)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    con->user = NULL;
    trace("arcanScreenFini");
}

static Status
ArcanKeyboardInit(KdKeyboardInfo * ki)
{
    ki->minScanCode = 0;
    ki->maxScanCode = 247;
    arcanInputPriv.ki = ki;

    return Success;
}

static Status
ArcanKeyboardEnable(KdKeyboardInfo * ki)
{
    return Success;
}

static void
ArcanKeyboardDisable(KdKeyboardInfo * ki)
{
    return;
}

static void
ArcanKeyboardFini(KdKeyboardInfo * ki)
{
}

static void
ArcanKeyboardLeds(KdKeyboardInfo * ki, int leds)
{
    trace("arcanKeyboardLeds(%d)", leds);
    struct arcan_event ev = (struct arcan_event){
        .ext.kind = ARCAN_EVENT(MESSAGE),
    };
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    snprintf(
        (char*)ev.ext.message.data, 78, "kind=kbdstate:led=%d", leds);
    ARCAN_ENQUEUE(con, &ev);
}

static void
ArcanKeyboardBell(KdKeyboardInfo * ki, int volume, int frequency, int duration)
{
/* find primary segment, enqueue as alert */
}

KdKeyboardDriver arcanKeyboardDriver = {
    "arcan",
    ArcanKeyboardInit,
    ArcanKeyboardEnable,
    ArcanKeyboardLeds,
    ArcanKeyboardBell,
    ArcanKeyboardDisable,
    ArcanKeyboardFini,
    NULL,
};
void
arcanCardFini(KdCardInfo * card)
{
    arcanPriv *priv = card->driver;

    trace("arcanCardFini");
    free(priv->base);
    free(priv);
}

void
arcanCloseScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    struct arcan_shmif_cont *acon = scrpriv->acon;
    trace("arcanCloseScreen");
    if (!scrpriv)
        return;

    scrpriv->acon->user = NULL;
    pScreen->CloseScreen = scrpriv->CloseHandler;
    free(scrpriv);
    screen->driver = NULL;
    (*pScreen->CloseScreen)(pScreen);
/* drop shmifext last as that will kill the GL context and thus all the
 * VAOs / VBOs etc. that glamour is using will be defunct */
    arcan_shmifext_drop(acon);
}

void
arcanPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs)
{
/*
 * FIXME:
 * should probably forward some cmap_entry thing and invalidate
 * the entire region. Somewhat unsure how this actually works
 */
    trace("arcanPutColors");
}

int
glamor_egl_fd_name_from_pixmap(ScreenPtr screen,
                               PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size)
{
    return 0;
}

void
arcanGetColors(ScreenPtr pScreen, int n, xColorItem * pdefs)
{
    while (n--) {
        pdefs->red = 8;
        pdefs->green = 8;
        pdefs->blue = 8;
        pdefs++;
    }
    trace("arcanGetColors");
}
