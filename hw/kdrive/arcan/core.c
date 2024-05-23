/*
 * Rough notes on what it is missing:
 *
 *  - to get better .utf8 handling for input, consider injecting
 *     an XIM and use that to provide the actual text. It can be just a
 *     placeholder / empty XIM and just set the last codepoint as the text.
 *
 *  - present clients only partially handled.
 *
 *  - output segment to pixmap substitution incomplete,
 *
 *           since it is mainly a compatibility thing with sharing / recording
 *           tools, the main concern is probably things using the root window
 *           to draw / compose into a surface the client can access.
 *
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <pthread.h>
#include <xkbsrv.h>
#include <xcb/xcb.h>
#include <sys/wait.h>
#include "compint.h"
#include "os/ddx_priv.h"
#include "os/osdep.h"
#include "dix_priv.h"

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
#include "propertyst.h"
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
#include "xa-present.h"
#include <misync.h>
#include "../../dix/enterleave.h"

arcanInput arcanInputPriv;
arcanConfig arcanConfigPriv;
static int mouseButtonBitmap;
static void
cmdCreateProxyWindow(
                       struct arcan_shmif_cont *con,
                       int x, int y, int w, int h, unsigned long vid, bool out);
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

arcanWindowPriv *arcanWindowFromWnd(WindowPtr wnd)
{
    return dixLookupPrivate(&wnd->devPrivates, &windowPriv);
}

arcanPixmapPriv *arcanPixmapFromPixmap(PixmapPtr pmap)
{
  return dixLookupPrivate(&pmap->devPrivates, &pixmapPriv);
}

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
    Atom _NET_WM_NAME;
    Atom _NET_ACTIVE_WINDOW;
    Atom UTF8_STRING;
    Atom WM_PROTOCOLS;
    Atom WM_DELETE_WINDOW;
    Atom WM_TAKE_FOCUS;
} wm_atoms;

static void resolveAtoms(void)
{
#define make(N) MakeAtom(N, strlen(N), true)
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
    wm_atoms._NET_WM_WINDOW_TYPE_DND = make("_NET_WM_WINDOW_TYPE_DND");
    wm_atoms._NET_ACTIVE_WINDOW = make("_NET_ACTIVE_WINDOW");
    wm_atoms._NET_WM_NAME = make("_NET_WM_NAME");
    wm_atoms.UTF8_STRING = make("UTF8_STRING");
    wm_atoms.WM_PROTOCOLS = make("WM_PROTOCOLS");
    wm_atoms.WM_DELETE_WINDOW = make("WM_DELETE_WINDOW");
    wm_atoms.WM_TAKE_FOCUS = make("WM_TAKE_FOCUS");
#undef make
}

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

/*
 * This is overly aggressive as it will overwrite any other Xresource value,
 * the 'proper' way to do it would be to parse and look for Xft.dpi
 * specifically and just overwrite that entry.
 */
static void updateXftDpi(arcanScrPriv* ascr)
{
    char buf[64];
    snprintf(buf, 64, "Xft.dpi:\t%.0f\n", arcan_init->density * 2.54);

    dixChangeWindowProperty(
        serverClient,
        ascr->screen->root,
        XA_RESOURCE_MANAGER,
        XA_STRING,
        8,
        PropModeReplace,
        strlen(buf) + 1,
        (void*) buf,
        false
    );
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

static bool isRedirectCandidate(arcanScrPriv *ascr, WindowPtr wnd)
{
    return wnd->parent && wnd->parent == ascr->screen->root && wnd->drawable.class == InputOutput;
}

arcanWindowPriv *ensureArcanWndPrivate(WindowPtr wnd)
{
    arcanWindowPriv *res = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    if (res)
        return res;

    res = malloc(sizeof(arcanWindowPriv));
    *res = (arcanWindowPriv){0};
    res->ident.category = EVENT_EXTERNAL;
    res->ident.ext.kind = ARCAN_EVENT(IDENT);
    dixSetPrivate(&wnd->devPrivates, &windowPriv, res);
    return res;
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
 * distinguishing type (just shmdesc->addr + offset) -
 *
 * For 'working' interpose then we would need to:
 *
 *   a. have a proxy drawable (that we get from PixmapFromSHMIF)
 *      that should be fairly easy, extract the windowpriv even if that is root.
 *
 *   b. Implement damage tracking for the interposition surface.
 *      It seems like XFixes ties a Region to the Client as a RegionResType.
 *      If so, we can then use the STEPFRAME event for them output segment
 *      and map its dirty region into that.
 *
 */
    return src;
}

static PixmapPtr arcanGetWindowPixmap(WindowPtr wnd)
{
    ScreenPtr pScreen = wnd->drawable.pScreen;
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr res = NULL;

/* Reject or substitute DispatchReqScr if the window has been marked as
 * protected. */
    arcanWindowPriv *apriv = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    if (apriv && apriv->shmif){
        arcanShmifPriv *shmif = apriv->shmif->user;
        if (shmif->bound){
            return shmif->pixmap;
        }
        else
            trace("getWindowPixmap(unbound)");
    }

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
           wnd->realized;
}

static void flagUnsynched(WindowPtr wnd)
{
    while (wnd && wnd->parent){
        wnd->unsynched = 1;
        wnd = wnd->parent;
    }
}

/* count number of siblings at the same depth, composition order is reversed
 * to order in arcan where draw order is low order to high */
static uint8_t getOrder(WindowPtr wnd)
{
    uint8_t order = 0;

    while (wnd->prevSib){
        wnd = wnd->prevSib;
        if (isWindowVisible(wnd))
            order = order + 1;
    }

    uint8_t count = 0;

    while (wnd->nextSib && count < 255){
        if (isWindowVisible(wnd))
            count = count + 1;
        wnd = wnd->nextSib;
    }

    return count - order;
}

static PropertyPtr getWindowProperty(WindowPtr wnd, ATOM match)
{
    if(!wnd->optional || !wnd->optional->userProps)
        return NULL;

    PropertyPtr cur = wnd->optional->userProps;
    while (cur){
        if (cur->propertyName == match)
            return cur;
        cur = cur->next;
    }

    return NULL;
}

static void dumpWindowProperties(WindowPtr wnd)
{
    if (!wnd->optional || !wnd->optional->userProps){
        trace("window(%d) - no properties", (int) wnd->drawable.id);
        trace("----------");
        return;
    }

    PropertyPtr cur = wnd->optional->userProps;

    trace("window(%d):", (int) wnd->drawable.id);
    while (cur){
        trace("\t%s : ", NameForAtom(cur->propertyName));
        if (cur->type == XA_STRING || cur->type == wm_atoms.UTF8_STRING){
            trace("\t\tstring(%d): %.*s", cur->size, cur->size, cur->data);
        }
        if (cur->type == XA_ATOM){
            trace("\tatom: %s, size: %d", NameForAtom(*(ATOM*)cur->data), (int) cur->size);
        }
        cur = cur->next;
        continue;
    }

    trace("----------");
}

static bool windowSupportsProtocol(WindowPtr wnd, Atom protocol)
{
    if (!wnd->optional || !wnd->optional->userProps){
        return false;
    }

    PropertyPtr proto = getWindowProperty(wnd, wm_atoms.WM_PROTOCOLS);
    if (!proto || proto->type != XA_ATOM)
        return false;

    return true;
}

static void sendWndData(WindowPtr wnd, int depth, int max_depth, bool redir, void *tag)
{
    struct arcan_shmif_cont *acon = tag;
    arcanWindowPriv *awnd = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    int bw = wnd->borderWidth;

    struct arcan_event out = (struct arcan_event)
    {
       .category = EVENT_EXTERNAL,
       .ext.kind = ARCAN_EVENT(VIEWPORT),
       .ext.viewport.w = wnd->drawable.width,
       .ext.viewport.h = wnd->drawable.height,
       .ext.viewport.x = wnd->drawable.x,
       .ext.viewport.y = wnd->drawable.y,
       .ext.viewport.border = {bw, bw, bw, bw},
       .ext.viewport.embedded = wnd->overrideRedirect,
       .ext.viewport.ext_id = wnd->drawable.id,
       .ext.viewport.order = wnd->visibility == VisibilityUnobscured ? getOrder(wnd) : -1,
       .ext.viewport.invisible = !isWindowVisible(wnd),
       .ext.viewport.focus = EnterLeaveWindowHasFocus(wnd),
       .ext.viewport.anchor_edge = true,
       .ext.viewport.edge = 1
    };

/* hopefully this is OK enough to solve the override-Redirect + transient ones without
 * adding any special cases so that more Arcan WMs just work without any considerations */
    PropertyPtr transient = getWindowProperty(wnd, XA_WM_TRANSIENT_FOR);
    if (transient){
        WindowPtr twnd;
        if (Success == dixLookupResourceByType((void**)&twnd,
                                               *(XID*) transient->data,
                                               RT_WINDOW, NULL, DixReadAccess)){
            out.ext.viewport.embedded = true;
        }
    }

    PropertyPtr name = getWindowProperty(wnd, wm_atoms._NET_WM_NAME);

/* shmif defines IDENT as single-part so we are locked to a short title */
    if (awnd && name && (name->type == XA_STRING || name->type == wm_atoms.UTF8_STRING)){
        size_t cap = sizeof(awnd->ident.ext.message.data) - 1;
        if (cap > name->size)
            cap = name->size;

        if (memcmp(awnd->ident.ext.message.data, name->data, cap) != 0){
            memcpy(awnd->ident.ext.message.data, name->data, cap);
            awnd->ident.ext.message.data[cap] = '\0';
            ARCAN_ENQUEUE(acon, &awnd->ident);
        }
    }

/* _NET_WM_TYPE_DND + XDndAware */

/* translate relative to anchor parent
 * this was removed as maintaining the stacking order didn't provide much utility
 * and can be reconstructed from stacking information anyhow
    if (anchorRel){
        out.ext.viewport.x = wnd->drawable.x - bw - anchorRel->drawable.x;
        out.ext.viewport.y = wnd->drawable.y - bw - anchorRel->drawable.y;
        out.ext.viewport.parent = anchorRel->drawable.id;
    }
*/

/* ensure 'correct' alpha flag, this might take another roundtrip to get right,
 * so a better tactic for these toggles should really be used */
    if (redir){
        if (out.ext.viewport.embedded){
            acon->hints &= ~SHMIF_RHINT_IGNORE_ALPHA;
        }
        else
            acon->hints |= SHMIF_RHINT_IGNORE_ALPHA;
        arcan_shmif_resize(acon, acon->w, acon->h);
    }

/* actually only send if the contents differ from what we sent last time */
    if (awnd){
        if (memcmp(&out, &awnd->ev, sizeof(struct arcan_event)) != 0){
            memcpy(&awnd->ev, &out, sizeof(struct arcan_event));
            ARCAN_ENQUEUE(acon, &out);
         }
    }

/* this workaround shold be reconsidered, right now it is only compositor redir
 * windows that get a private dixEntry and we instead have the unsynched prop
 * as part of windowstr.h - the main path that would hit this is for wmSynch
 * with forced metadata based decomposition. */
    else
        ARCAN_ENQUEUE(acon, &out);

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

/* find a free group/slot and store the segment in there - the int64
 * indirection is to have a tristate yet still be able to follow with through
 * all the layers. , < 0 for error conditions then split the bits into a group
 * and a slot (of 64) while still having an allocation bitmap for nicer
 * cache behaviour and debugging/inspection vs. lists
 */
static int64_t sendSegmentToPool(arcanScrPriv *S, struct arcan_shmif_cont *C)
{
    uint64_t i = __builtin_ffsll(~S->redirectBitmap);
    if (0 == i){
        arcan_shmif_drop(C);
        return false;
    }
    i--;
    S->redirectBitmap |= (uint64_t) 1 << i;
    S->pool[i] = C;
    InputThreadRegisterDev(C->epipe, arcanFlushRedirectedEvents, C);

    return i;
}

static void dumpPool(const char* tag, arcanScrPriv *S)
{
    printf("Pool(%s):\n", tag);
    for (size_t i = 0; i < 64; i++){
        if (!S->pool[i])
            continue;

    struct arcan_shmif_cont *C = S->pool[i];
    arcanShmifPriv *P = C->user;
    if (!P->bound){
        printf("%s: pool[%zu] - not bound\n", tag, i);
        continue;
    }
    printf("pool[%zu] (%zu*%zu) @ %"PRIxPTR":\n", i, C->w, C->h, (uintptr_t)C->vidp);
    if (P->window){
        printf("\twindow(%"PRIxPTR") -> %zu, %zu*%zu\n",
        (uintptr_t) P->window, (size_t) P->window->drawable.id,
        (size_t) P->window->drawable.width, (size_t) P->window->drawable.height);
    }
    else
        printf("\twindow(UNMAPPED)\n");

    if (P->pixmap){
        printf("\tpixmap(%"PRIxPTR") -> %zu*%zu\n\n",
            (uintptr_t) P->pixmap,
            (size_t) P->pixmap->drawable.width,
            (size_t)P->pixmap->drawable.height
        );
    }
    else
        printf("\tpixmap(UNMAPPED)\n\n");
    }
}

/*
 * using a 64bit identifier to use lower-32bits for pool and upper for group
 */
static struct arcan_shmif_cont* segmentIDtoContext(arcanScrPriv *S, uint32_t i)
{
    if (i < 64)
        return S->pool[i];

    return NULL;
}

/*
 * match against w / h to not trigger a resize or pick a designated pWin if
 * one is reserved (in the case of server-initiated dynamic redirect)
 */
static int64_t findBestSegment(arcanScrPriv *S, int w, int h, WindowPtr pWin)
{
    uint64_t bmap = S->redirectBitmap;
    int64_t bi = -1;
    size_t best_pxd = -1;

    while (bmap){
        uint64_t i = __builtin_ffsll(bmap) - 1;
        struct arcan_shmif_cont *C = S->pool[i];
        arcanShmifPriv *P = C->user;

/*
 * This one is interesting, if we go for a segment preallocated for a window but
 * not still in use there will be at least n_wnd+1 segments in flight, on the other
 * hand the resize becomes de-facto asynch.
 *
 * This moves the problem of handling two segments mapped to the same window to
 * the arcan-appl side and otherwise we introduce a resize like stall. The
 * crutch is that the swapping behaviour is needed on arcan side regardless of
 * pool reallocations. If we do return a bound surface here, another problem
 * persists that would need handling and that is the CopyArea call that the
 * COMPOSITE extension applies, as it smears out its own pixmap.
 */
        if (pWin && P->window == pWin /* && !P->bound */){
            return i;
        }
/* otherwise aim for the shortest size distance, hoping that the delta
 * will be within the resize-skip threshold - prefer a complete match */
        else if (!P->bound){
              size_t pxd = fabs((C->w * C->h - w * h) - best_pxd);
              if (pxd < best_pxd){
                bi = i;
                best_pxd = pxd;
                if (pxd == 0)
                    break;
              }
        }
        bmap &= ~((uint64_t)1 << i);
    }

    return bi;
}

/*
 * Enable / Disable VBlank and presentation feedback reporting. Both yield
 * STEPFRAME events (like with the render hint of frame delivery) but latched
 * to slightly different clocks. Blank is for aligning, present for estimating
 * pipeline latency.
 */
static void setAprivVblankPresent(
                                  struct arcan_shmif_cont *C,
                                  arcanShmifPriv *apriv, Bool state)
{
    arcan_event ev = {
        .ext.kind = ARCAN_EVENT(CLOCKREQ)
    };

    if (state != apriv->vblank){
        apriv->vblank = state;
        ev.ext.clock.dynamic = 1;
        ARCAN_ENQUEUE(C, &ev);
    }

    if (state != apriv->present){
        apriv->present = state;
        ev.ext.clock.dynamic = 2;
        ARCAN_ENQUEUE(C, &ev);
    }
}

static Bool arcanRealizeWindow(WindowPtr wnd)
{
    Bool rv = true;
    arcanScrPriv *ascr = getArcanScreen(wnd);
    trace("realizeWindow(%d)", (int) wnd->drawable.id);
/*  dumpWindowProperties(wnd); */

/* For -exec mode, remember the number of clients when the first actual window
 * gets presented and use that as a bound to determine when to timeout and
 * terminate. This should cover clients that exit but leaves d-bus and similar
 * vermin around with active connections to an otherwise dead Xserver */
    if (!arcanConfigPriv.clientShutdownBound){
        arcanConfigPriv.clientShutdownBound = currentMaxClients;
    }

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
    if (ascr->wmSynch){
        snprintf(
                 (char*)ev.ext.message.data, 78,
                 "kind=realize:xid=%d",
                 (int) wnd->drawable.id
        );
        ARCAN_ENQUEUE(ascr->acon, &ev);
        flagUnsynched(wnd);
    }

    if (!ascr->defaultRootless)
        return rv;

/* Might need special treatment / tracking if the window is an overlay */
    if (!wnd->parent){
        BoxRec box = {0, 0, ascr->acon->w, ascr->acon->h};
        RegionReset(&wnd->winSize, &box);
        RegionNull(&wnd->clipList);
        RegionNull(&wnd->borderClip);
    }

/* Realize private tracking structure for dirty/metadata synch */
    arcanWindowPriv *ext = ensureArcanWndPrivate(wnd);
    if (!isRedirectCandidate(ascr, wnd))
        return rv;

    arcan_shmif_mousestate_setup(ascr->acon, false, NULL);

/* compRedirect will create a pixmap and we do that out of a fitting per-toplevel
 * shmif segment (or request one if there is nothing in the reuse pool) */
    trace("redirect:window=%"PRIxPTR, wnd);
    compRedirectWindow(serverClient, wnd, CompositeRedirectManual);
    ext->damage = DamageCreate(NULL, NULL,
                               DamageReportNone, TRUE, ascr->screen, NULL);
    DamageRegister(&wnd->drawable, ext->damage);
    DamageSetReportAfterOp(ext->damage, true);

    return rv;
}

static Bool arcanUnrealizeWindow(WindowPtr wnd)
{
    arcanScrPriv *ascr = getArcanScreen(wnd);
    Bool rv = true;

    if (arcanConfigPriv.present){
        struct xa_present_window *pw = xa_present_window_priv(wnd);
        if (pw)
            xa_present_unrealize_window(pw);
    }

/* find the pixmap, shmif window triple and mark it as free for re-use. */
    arcanWindowPriv *apriv = dixLookupPrivate(&wnd->devPrivates, &windowPriv);
    if (apriv){
        if (apriv->shmif){
            arcanShmifPriv *shmif = apriv->shmif->user;
            sendWndData(wnd, 0, 0, true, apriv->shmif);
            shmif->window = NULL;
            trace("unrealize:id=%zu:shmif=yes:pixmap=%"PRIxPTR":window=%"PRIxPTR,
                  wnd->drawable.id, (uintptr_t) shmif->pixmap,
                  (uintptr_t) shmif->window
                 );
            shmif->bound = false;
            if (arcanConfigPriv.present)
                setAprivVblankPresent(apriv->shmif, shmif, false);
        }
        else
            trace("unrealize:id=%zu:shmif=no", (size_t) wnd->drawable.id);

        if (apriv->damage){
            DamageDestroy(apriv->damage);
            apriv->damage = NULL;
        }

        free(apriv);
        dixSetPrivate(&wnd->devPrivates, &windowPriv, NULL);
    }
    else
        trace("unrealize:unknown:id=%zu", (size_t)wnd->drawable.id);

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

    flagUnsynched(wnd);
    if (oldNextSibling)
        flagUnsynched(oldNextSibling);

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

    if (arcanConfigPriv.present)
        xa_present_cleanup(wnd);

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

    trace("kind=ConfigureWindow:x=%d:y=%d:w=%d:h=%d:mapped=%d:realized=%d",
          x, y, w, h, (int)wnd->mapped, (int)wnd->realized);
    int res = 0;

    if (ascr->hooks.configureWindow){
        ascr->screen->ConfigNotify = ascr->hooks.configureWindow;
        res = ascr->hooks.configureWindow(wnd, x, y, w, h, bw, sibling);
        ascr->screen->ConfigNotify = arcanConfigureWindow;
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
    if (arcanConfigPriv.glamor)
        setGlamorMask(screen);
    else
        setArcanMask(screen);

    FakeScreenFps = arcan_init->rate;

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
TranslateInput(
    struct arcan_shmif_cont* con, arcan_event* oev, arcanShmifPriv *P)
{
    int x = 0, y = 0;
    arcan_ioevent *ev = &oev->io;

/* If P is provided the input is on a redirected segment, meaning that even the absolute
 * coordinates are absolute in the space of the segment not some unknown global. Anchorhints
 * will still have the windows configured at the right position.
 */
    int wnd_xofs = 0;
    int wnd_yofs = 0;
    if (P && P->window){
        wnd_xofs = P->window->drawable.x;
        wnd_yofs = P->window->drawable.y;
    }

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
            else if (arcan_shmif_mousestate(con, NULL, oev, &x, &y)){
                ValuatorMask mask;
                int valuators[3] = {x+wnd_xofs, y+wnd_yofs, 0};
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
        size_t count = 0;
        for (size_t i = 0; i < sizeof(code_tbl) / sizeof(code_tbl[0]); i++){
            if (code_tbl[i]){
                trace("force-release:%d", code_tbl[i]);
                enqueueKeyboard(i, 0);
                code_tbl[i] = 0;
                count++;
            }
        }
        trace("unfocus, release: %zu keys\n", count);
        KdEnqueuePointerEvent(arcanInputPriv.pi, KD_MOUSE_DELTA, 0, 0, 0);
    }

    if (arcanConfigPriv.no_dynamic_resize)
        return;

    if (w >= 640 && h >= 480 && (con->w != w || con->h != h) && !(fl & 1)){
#ifdef RANDR
       if (ppcm == 0 && arcan_init)
           ppcm = arcan_init->density;

       if (ppcm > 0.0 && arcan_init && arcan_init->density != ppcm){
           arcan_init->density = ppcm;
           updateXftDpi(apriv);
       }

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

/*
 * terminate after 10 seconds of no active clients, only used for -exec
 */
static CARD32
shutdownTimerCheck(OsTimerPtr timer, CARD32 time, void *arg)
{
    if (!arcanConfigPriv.clientShutdownBound ||
        currentMaxClients > arcanConfigPriv.clientShutdownBound){
            arcanConfigPriv.timeout = 10;
            return 1000;
    }

    if (arcanConfigPriv.timeout){
        arcanConfigPriv.timeout--;
        if (!arcanConfigPriv.timeout){
            CloseWellKnownConnections();
            OsCleanup(1);
            dispatchException |= DE_TERMINATE;
            return 0;
        }
    }
    else
        arcanConfigPriv.timeout = 10;

    return 1000;
}

static
void wmSendMeta(WindowPtr wnd, int depth, int max_depth, void *tag)
{
    if (wnd->unsynched)
        sendWndData(wnd, depth, max_depth, false, tag);
}

static
void
arcanSignal(struct arcan_shmif_cont* con)
{
    arcanScrPriv *scrpriv = con->user;

    if (!scrpriv->dirty)
        return;

    scrpriv->unsynched = 0;
    if (scrpriv->wmSynch)
        synchTreeDepth(scrpriv, scrpriv->screen->root, wmSendMeta, true, con);

/* To avoid a storm of event -> update, don't signal or attempt to update
 * before the segment has finished synchronising the last one. */
    if (arcan_shmif_signalstatus(con))
        return;

    RegionPtr region;
    bool in_glamor = false;

    arcanSynchCursor(scrpriv, false);

    region = DamageRegion(scrpriv->damage);
    if (!RegionNotEmpty(region) || scrpriv->defaultRootless)
        return;

    BoxPtr box = RegionExtents(region);
    arcan_shmif_dirty(con, box->x1, box->y1, box->x2, box->y2, 0);

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
                       int x, int y, int w, int h, unsigned long vid, bool out)
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

    pthread_create(&pth, &pthattr,
                  out ?
                  (void*)(void*)arcanProxyContentWindowDispatch :
                  (void*)(void*)arcanProxyWindowDispatch,
                  proxy);
}

void arcanForkExec(void)
{
/* This one is quite annoying as neither xcb nor xlib permits is to specify a
 * process-inherited descriptor to the socket which would really be the proper
 * thing to do. */
    if (!arcanConfigPriv.exec)
        return;

    char* const argv[] = {
          strdup("/bin/sh"),
          strdup("-c"),
          strdup(arcanConfigPriv.exec),
          NULL
    };

/* Even if a child dies we can't use that as a trigger to shut down as it could
 * fork over to another process that takes time to spawn and so on. Therefore
 * the safest best is to block re-executing on reset and use a timer to check
 * for some arbitrary n-Seconds without a new client connecting, let's die. */
    arcanConfigPriv.timeout = 0;
    arcanConfigPriv.exec = NULL;
    TimerSet(arcanConfigPriv.shutdownTimer, 0, 1000, shutdownTimerCheck, NULL);
    trace("ready at : %d\n", currentMaxClients);

    pid_t pid = fork();
    if (-1 == pid){
        ErrorF("couldn't spawn wm child\n");
        return;
    }

    if (0 == pid){
        if (fork() != 0){
            exit(EXIT_SUCCESS);
        }

      char buf[16];
        snprintf(buf, 16, ":%s", display);
        setenv("DISPLAY", buf, 1);
        unsetenv("ARCAN_CONNPATH");
/* option here is to do something else with stdio to make troubleshooting
 * easier, setup new pipes and keep them around if the parent requests it. */
        setsid();
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);

        execv("/bin/sh", argv);
        exit(EXIT_FAILURE);
    }

    free(argv[0]);
    free(argv[1]);
    free(argv[2]);
}

void
arcanNotifyReady(void)
{
   updateXftDpi(arcan_shmif_primary(SHMIF_INPUT)->user);
   arcanForkExec();
}

static
void
cmdClipboardWindow(struct arcan_shmif_cont *con, bool paste)
{
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, AF_UNIX, pair);

    trace("clipboard");
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
cmdRaiseWindow(XID id)
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

  int mask = CWStackMode;
  XID vlist[2] = {Above};
  ConfigureWindow(res, mask, vlist, client);
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

    int mask = 0;
    if (w && h){
        int bw = res->borderWidth * 2;
        w += bw;
        h += bw;
        mask |= CWWidth | CWHeight;
    }

/* Fake a message to ourselves, seems like there never was a real internal way
 * of moving/configuring a window - should perhaps add that instead of these
 * kinds of hacks. */
    if (xy){
        XID vlist[4] = {x, y, w, h};
        mask |= CWX | CWY;
        ConfigureWindow(res, mask, vlist, client);
    }
    else{
        XID vlist[2] = {w, h};
        ConfigureWindow(res, mask, vlist, client);
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

static void
announceRequestBuiltins(struct arcan_shmif_cont *con)
{
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
            cmdCreateProxyWindow(con, x, y, w, h, id, false);
        else
            cmdReconfigureWindow(x, y, w, h, id, got_xy);
    }
    else if (strcmp(kind, "destroy") == 0 && id >= 0){
        FreeResource(id, RT_NONE);
    }
    else if (strcmp(kind, "resource") == 0){
    }

cleanup:
    arg_cleanup(cmd);
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
    pPixmap->drawable.bitsPerPixel = 32;
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

    trace("(newPixmap) %d * %d @ %"PRIxPTR, (int) C->w, (int) C->h, (uintptr_t) pPixmap);
    return pPixmap;
}

static void
handleNewSegment(struct arcan_shmif_cont *C, arcanScrPriv *S, int kind, bool out, unsigned id)
{
/* Create a window where we map our contents into the PIXMAP, this is done as a
 * separate thread and a discrete X client. If there is a specific ID provided,
 * we should instead substitute read/draw requests by merely marking it as the
 * proxy pixmap for that window. */
    if (out){
        struct arcan_shmif_cont *con = malloc(sizeof(struct arcan_shmif_cont));
        *con = arcan_shmif_acquire(C, NULL, SEGID_ENCODER, SHMIF_DISABLE_GUARD);
        cmdCreateProxyWindow(con, 0, 0, con->w, con->h, id, out);
    }
    else if (kind == SEGID_CLIPBOARD || kind == SEGID_CLIPBOARD_PASTE){
        struct arcan_shmif_cont *clip = malloc(sizeof(struct arcan_shmif_cont));
        *clip = arcan_shmif_acquire(C, NULL, kind, SHMIF_DISABLE_GUARD);
        cmdClipboardWindow(clip, kind == SEGID_CLIPBOARD_PASTE);
    }

/* allocate the cursor and set its initial state to still use the system default with the
 * custom drawn cursor disabled until we get a setcursor call that would toggle it. */
    else if (kind == SEGID_CURSOR){
        if (S->cursor){
            arcan_shmif_drop(S->cursor);
            S->cursor = NULL;
        }
        S->cursor = malloc(sizeof(struct arcan_shmif_cont));
        if (S->cursor){
            *(S->cursor) = arcan_shmif_acquire(C, NULL, SEGID_CURSOR, SHMIF_DISABLE_GUARD);

            arcan_shmif_enqueue(S->cursor, &(arcan_event){
                 .ext.kind = ARCAN_EVENT(CURSORHINT),
                 .ext.message.data = "default"
            });

            S->cursor_event.ext.kind = ARCAN_EVENT(VIEWPORT);
            S->cursor_event.ext.viewport.invisible = true;
            arcan_shmif_enqueue(S->cursor, &S->cursor_event);
         }
    }

/* Someone has explicitly told us to redirect a single window tree. This is
 * treated as a special case to the rootless style 'redirect everything': we
 * add the segment to the pool and tag it with the destination window. Then the
 * composite extension sets up its redirection and a corresponding pixmap will
 * be created that maps into the segment. */
    else if (id){
        WindowPtr wnd;
        if (Success != dixLookupResourceByType((void**) &wnd, id, RT_WINDOW, NULL, DixWriteAccess)){
            trace("redirect on unknown ID");
            return;
        }

        struct arcan_shmif_cont *pxout = malloc(sizeof(struct arcan_shmif_cont));
        arcanShmifPriv* contpriv = malloc(sizeof(arcanShmifPriv));
        *contpriv = (arcanShmifPriv){
            .bound = false,
            .window = wnd
        };
        *pxout = arcan_shmif_acquire(C, NULL, kind, SHMIF_DISABLE_GUARD);
        pxout->user = contpriv;

        sendSegmentToPool(S, pxout);
        compRedirectWindow(serverClient, wnd, CompositeRedirectManual);
    }
}

static int64_t requestIntoPool(arcanScrPriv* ascr, XID id, int w, int h)
{
    ARCAN_ENQUEUE(ascr->acon, &(struct arcan_event){
        .ext.kind = ARCAN_EVENT(SEGREQ),
        .ext.segreq.width = w,
        .ext.segreq.height = h,
        .ext.segreq.kind = SEGID_BRIDGE_X11,
        .ext.segreq.id = id
    });

    arcan_event ev;
    while (arcan_shmif_wait(ascr->acon, &ev)){
        int status = arcanEventDispatch(ascr->acon, ascr, &ev, id);
        if (status == -2){
            return -1;
        }
        else if (status == 2){
            struct arcan_shmif_cont *C = malloc(sizeof(struct arcan_shmif_cont));
            if (!C)
                return -1;

/* We can do without the guard on these ones as:
 *     a. we don't use signal to block,
 *     b. don't use a blocking event loop */
            *C = arcan_shmif_acquire(ascr->acon,
                                     NULL,
                                     SEGID_BRIDGE_X11,
                                     SHMIF_DISABLE_GUARD);
            if (!C->addr){
                free(C);
                return -1;
            }
            return sendSegmentToPool(ascr, C);
        }
    }

    return -1;
}

static Bool arcanDestroyPixmap(PixmapPtr pixmap)
{
/* if the pixmap is mapped to shmif, remove our privatePtr, mark the segment
 * for re-use in the pool */
    Bool retv = true;
    KdScreenPriv(pixmap->drawable.pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *ascr = screen->driver;
    arcanPixmapPriv *ext = dixLookupPrivate(&pixmap->devPrivates, &pixmapPriv);

/* remove the reference to the triple */
    if (ext){
/*      dumpPool("destroyPixmap", ascr); */
        struct arcan_shmif_cont *C = segmentIDtoContext(ascr, ext->id);
        if (ext->tmpbuf){
            free(ext->tmpbuf);
            ext->tmpbuf = NULL;
        }
        else
        if (C && C->user){
            arcanShmifPriv *contpriv = C->user;
            trace("destroyPixmap:backing=shmif:window=%"PRIxPTR, (uintptr_t) contpriv->window);
            contpriv->pixmap = NULL;
            contpriv->bound = false;
            contpriv->dying = 0;
        }
        else
            ErrorF("destroyPixmap:backing=no");
    }

    if (ascr->hooks.destroyPixmap){
        ascr->screen->DestroyPixmap = ascr->hooks.destroyPixmap;
        retv = ascr->hooks.destroyPixmap(pixmap);
        ascr->screen->DestroyPixmap = arcanDestroyPixmap;
    }

    return retv;
}

static void loadKeymapFromTextFile(int fd)
{
    trace("got new keymap");
    FILE* fpek = fdopen(fd, "r");
    if (!fpek)
        return;

    char* keystr;
    size_t size;

    FILE* fmem = open_memstream(&keystr, &size);
    if (fmem){
        while (!feof(fpek)){
            char buf[4096];
            size_t nr = fread(buf, 1, 4096, fpek);
            if (nr)
                fwrite(buf, nr, 1, fmem);
      }

      fclose(fmem);
      fclose(fpek);
   }
   else {
        trace("couldn't cache keymap");
        fclose(fpek);
        return;
   }

   DeviceIntPtr dev = arcanInputPriv.ki->dixdev;
   XkbDescPtr xkb = XkbCompileKeymapFromString(dev, keystr, size - 1);
    XkbChangesRec changes = {0};
    XkbUpdateDescActions(xkb, xkb->min_key_code, XkbNumKeys(xkb), &changes);
    XkbDeviceApplyKeymap(dev, xkb);
    DeviceIntPtr master = GetMaster(dev, MASTER_KEYBOARD);
    if (master && master != dev)
        XkbDeviceApplyKeymap(master, xkb);
    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
    free(keystr);
}

/* Grab a shmif context from the pool, unless it is from a subwindow to one
 * that is supposed to be redirected. In that case return NULL and the regular
 * screen based pixmap allocation will be used. The bigger caveat here is that
 * the invocation order comp->new->realize to comp->resize->new->destroy(old)
 * means that we will overallocate one and the window will have two shmif
 * pixmaps instead of the one + resize.*/
static PixmapPtr arcanCompNewPixmap(WindowPtr pWin, int x, int y, int w, int h)
{
/* First try and grab a segment intended for pWins drawable */
    if (!pWin->parent || pWin->parent->parent)
        return NULL;

/* on Resize->Reconfigure->(different w/h)->compose will have a reference to
 * the last pixmap that is already bound to a shmif context in order to copy
 * back and forth. This means that we'll either get the headaches of aliasing,
 * or pay the price of an extra shmif context in use. If we hide-viewport the
 * old context here we get flickering and if we don't we can get one or more
 * frames with both being visible. What we really want is to tell the first
 * context the same time another signals. */

/*
 * another option is simply resizing the original segment blocking the entire
 * copy chain from triggering, but that seem to be a more difficult dive than
 * expected, this should be roughly the process but something is amiss:
    struct arcan_shmif_cont *C = aWnd->shmif;
    CompWindowPtr cw = GetCompWindow(pWin);
    arcan_shmif_resize(aWnd->shmif, w, h);
    pWin->drawable.pScreen->ModifyPixmapHeader(pmap,
        C->w, C->h, 24, BitsPerPixel(24), C->stride, C->vidp);
    cw->pOldPixmap = NULL;
    return pmap;
    }
*/

    arcanWindowPriv *aWnd = ensureArcanWndPrivate(pWin);
    arcanScrPriv *ascr = getArcanScreen(pWin);
    int64_t i = findBestSegment(ascr, w, h, pWin);
    struct arcan_shmif_cont *C = NULL;

/* Then just take any that is free and go for best for for w * h */
    if (-1 == i)
        i = findBestSegment(ascr, w, h, NULL);

    if (-1 == i)
        i = requestIntoPool(ascr, pWin->drawable.id, w, h);

/* Server side can't handle more requests, give up */
    if (-1 == i)
        return NULL;

/* bind the segment to a pixmap and attach damage tracking so that the _signal
 * calls only update relevant areas */
    C = segmentIDtoContext(ascr, i);
    trace("new_pixmap:id=%d:slot=%zu:x=%d:y=%d:w=%d:h=%d:shmif_w=%d:shmif_h=%d",
          (int) pWin->drawable.id, (size_t) i, x, y, w, h, C->w, C->h);

/* ensure the tracking structure for our window, pixmap and segment triple,
 * needed with the pixmapFromShmif allocation later */
    if (!C->user){
        arcanShmifPriv *contpriv = malloc(sizeof(arcanShmifPriv));
        *contpriv = (arcanShmifPriv){.bound = false};
        C->user = contpriv;
        trace("new_pixmap:bind_new");
    }

    arcanShmifPriv *shmif = C->user;

/* If we are reusing a shmif context but binding it to a new pixmap, we need a
 * safe copy so that when we return the pixmap to COMPOSITE there is no alias
 * to the same memory or its own copy-during-resize code will corrupt. This
 * copy is then checked for during DestroyPixmap */
    if (shmif->bound){
        trace("new_pixmap:backup_reuse");
        size_t nb = C->stride * C->h;
        arcanPixmapPriv *priv = dixLookupPrivate(
                                               &shmif->pixmap->devPrivates,
                                               &pixmapPriv);
        priv->tmpbuf = malloc(nb);
        memcpy(priv->tmpbuf, C->vidb, nb);
        pWin->drawable.pScreen->ModifyPixmapHeader(shmif->pixmap, 0, 0, 0, 0, 0, priv->tmpbuf);
    }

/* Default hints mode is this, then for overrideRedirect and similar surfaces
 * we do re-enable alpha. The basis for this is that alpha channel is a bit of
 * a free-for-all in X11 and with it enabled, firefox subsurfaces etc. would
 * appear black rather than have a blended shadow, but xterm/xeyes etc. would
 * not have any visible background. */
    C->hints = SHMIF_RHINT_ORIGO_UL    | SHMIF_RHINT_SUBREGION     |
               SHMIF_RHINT_CSPACE_SRGB | SHMIF_RHINT_IGNORE_ALPHA;

/* This gives us STEPFRAME events latched to frame delivery and combines with
 * the CLOCKREQ events when the segment is added to the pool. Combined it gives
 * us events with feedback on MSC and VBLANK to hook into present feedback. */
    if (arcanConfigPriv.present){
        C->hints |= SHMIF_RHINT_VSIGNAL_EV;
        setAprivVblankPresent(C, C->user, true);
    }

    arcan_shmif_resize(C, w, h);
    arcan_shmif_dirty(C, 0, 0, w, h, 0);
    shmif->dying = 0;

    PixmapPtr pmap = pixmapFromShmif(
                                     pWin->drawable.pScreen,
                                     C, CREATE_PIXMAP_USAGE_BACKING_PIXMAP
                                    );

    memset(C->vidb, '\0', C->stride * C->h);

    arcanPixmapPriv *aPix = malloc(sizeof(arcanPixmapPriv));
    *aPix = (arcanPixmapPriv){.id = i};
    dixSetPrivate(&pmap->devPrivates, &pixmapPriv, aPix);

   shmif->pixmap = pmap;
   shmif->window = pWin;

/* cause a viewport to be emitted before the frame signal */
   pWin->unsynched = true;
   shmif->bound = true;
   aWnd->shmif = C;
   dumpPool("newPixmap", ascr);

   return pmap;
}

/* property.x: PrintPropertys (#ifdef notdef)
 * window->optional->userProps
 *
 *  -> PropertyPtr window->optional->userProps
 *     p->propertyName == ATOM
 *        p->type == XA_STRING
 *        check data for Xft.dpi
 *        p = p->next
 *
 * then p->data
 */

static void resetRecover(struct arcan_shmif_cont *con, arcanScrPriv *ascr)
{
/* Hide root if we are in redirected mode */
    trace("resetRecover");
    if (arcanConfigPriv.redirect){
        trace("resetRecoverRedirect");
        ARCAN_ENQUEUE(ascr->acon, &(struct arcan_event){
                      .category = EVENT_EXTERNAL,
                      .ext.kind = ARCAN_EVENT(VIEWPORT),
                      .ext.viewport.invisible = true
        });
    }

    input_unlock();
        InputThreadUnregisterDev(con->epipe);
        InputThreadRegisterDev(con->epipe, arcanFlushEvents, con);

/* Take a copy of the redirect bitmap as is, remove each entry, repeat the
 * initial blocking requestIntoPool (which will fill the previously occupied
 * slot). */
        uint64_t bmap = ascr->redirectBitmap;
        while (bmap){
            uint64_t i = __builtin_ffsll(bmap) - 1;
            bmap &= ~((uint64_t)1 << i);
            ascr->redirectBitmap &= ~((uint64_t)1 << i);

            struct arcan_shmif_cont *C = ascr->pool[i];
            arcanShmifPriv *P = C->user;
            P->window->unsynched = 1;

/* PRESENT-note: For surfaces with a DMA buffer backing store we would need to
 * force an update via exposures and drop pending PRESENT state as those
 * callbacks won't fire on the segment.
 * arcanWindowPriv *awnd = dixLookupPrivate(&P->window->devPrivates, &windowPriv);
 */

/* Complex dance now is to swap out the backing of the Pixmap into the new
 * shmif context. */
/*            int64_t state = requestIntoPool( */
/* copy old into new, drop old, swap pixmap unless GBM, signal transfer asynch */
        }

        announceRequestBuiltins(con);
    input_lock();

    ascr->dirty = 1;
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
arcanEventDispatch(
                   struct arcan_shmif_cont *con,
                   arcanScrPriv *ascr,
                   arcan_event *aev, int64_t uid)
{
    struct arcan_event ev = *aev;
    if (ev.category == EVENT_IO){
        TranslateInput(con, &ev, NULL);
        return 0;
    }
    else if (ev.category != EVENT_TARGET)
        return 0;

    switch (ev.tgt.kind){
    case TARGET_COMMAND_STEPFRAME:
    if (ascr->wmSynch)
        synchTreeDepth(ascr, ascr->screen->root, wmSendMeta, true, con);
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
            resetRecover(con, ascr);
            if (0)
/* swap-out monitored FD */
        case 3:
            trace("parent_crash_recover");
            return 1;
        break;
        }
/* all the window metadata need to be present as create, and the ones that
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
    case TARGET_COMMAND_BCHUNK_IN:{
        if (strcmp(ev.tgt.message, "xkb") == 0){
            loadKeymapFromTextFile(ev.tgt.ioevs[0].iv);
        }
    }
    break;
    case TARGET_COMMAND_BCHUNK_OUT:
        dumpTree(ascr,
                 arcan_shmif_dupfd(ev.tgt.ioevs[0].iv, -1, false),
                 ev.tgt.message
                );
    break;
    case TARGET_COMMAND_REQFAIL:
        trace("segment-request rejected");
        return -2;
    break;
    case TARGET_COMMAND_NEWSEGMENT:
        if (uid > 0 && ev.tgt.ioevs[3].uiv == uid){
            return 2;
        }
        else{
            input_unlock();
            handleNewSegment(con,
                             con->user,
                             ev.tgt.ioevs[2].iv,
                             ev.tgt.ioevs[1].iv,
                             ev.tgt.ioevs[3].uiv);
            input_lock();
        }
    break;
    case TARGET_COMMAND_EXIT:
        return -1;
    break;
    default:
    break;
    }

    return 0;
}

void
arcanFlushEvents(int fd, int mask, void* tag)
{
    struct arcan_shmif_cont* con = tag;
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
      arcanSignal(con);

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
        ErrorF("multiple screen support still missing\n");
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
    arcanSignal(scrpriv->acon);
}

static Bool arcanSetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pPixmap = NULL;

    scrpriv->damage = DamageCreate((DamageReportFunc) NULL,
                                   (DamageDestroyFunc) NULL,
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
    arcanPixmapPriv *ext_pixmap;
    uintptr_t adisp, actx;
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
    ext_pixmap = malloc(sizeof(arcanPixmapPriv));
    if (!ext_pixmap){
        trace("ArcanDRI3PixmapFromFD()::Couldn't allocate pixmap metadata");
        gbm_bo_destroy(bo);
        glamor_destroy_pixmap(pixmap);
        return NULL;
    }

    *ext_pixmap = (arcanPixmapPriv){0};
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

        SetRootClip(pScreen, scrpriv->clip_mode);
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

    announceRequestBuiltins(con);
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

        if (size.mmWidth && size.mmHeight)
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

    RRScreenSize size = {
        .width = arcan_init->display_width_px,
        .height = arcan_init->display_height_px,
        .mmWidth = (float)arcan_init->display_height_px / (0.1 * arcan_init->density),
        .mmHeight = (float)arcan_init->display_height_px / (0.1 * arcan_init->density)
    };

    RRScreenSetSizeRange(
        pScreen,
        640, 480,
        arcan_init->display_width_px, arcan_init->display_height_px
    );
    RRModePtr mode = arcan_cvt(
                               arcan_init->display_width_px,
                               arcan_init->display_height_px,
                               (float) arcan_init->rate / 1000.0, 0, 0
                              );
    RROutputSetModes(scrpriv->randrOutput, &mode, 1, 1);
    RRScreenSizeNotify(pScreen);
    RROutputSetPhysicalSize(scrpriv->randrOutput, size.mmWidth, size.mmHeight);

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
    arcanPixmapPriv *ext_pixmap = dixLookupPrivate(
                                                   &pixmap->devPrivates,
                                                   &pixmapPriv);

    if (!ext_pixmap || !ext_pixmap->bo){
        trace("ArcanDRI3FdFromPixmap (%"PRIxPTR") -> reject", (uintptr_t) pixmap);
        return 0;
    }

    trace("ArcanDRI3FdFromPixmap (%"PRIxPTR")", (uintptr_t) pixmap);
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
    if (!width || !height || !num_fds || depth < 15 ||
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
    else
        return NULL;

    PixmapPtr res = boToPixmap(pScreen, bo, depth);
    trace("ArcanDRI3PixmapFromFD() => %"PRIxPTR, (uintptr_t) res);

    return res;
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
/*
 *  this information 'should' be present in the shmif extdev setup,
 *  but we have no way of querying / extracting in the DEVICEHINT as
 *  is.
 */
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

/*
 * syncobj notes:
 *    export: drmSyncobjCreate(dfd, 0, &uint32obj);
 *            drmSyncobjTransfer(dfd, uint32obj, 0, sobj->handle, uint64point, 0)
 *            drmSyncobjExportSyncFile(dfd, uint32obj, &intfd)
 *            drmSyncobjDestroy(dfd, uint32obj);
 *
 *    check:  drmSyncobjTimelineWait(dfd, &sobj->handle, &uint64point, 1,
 *                                   timeout::0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)
 *
 *                                   or DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
 *
 *    import: struct dma_buf_import_sync_file import {.fd = fd, .flags = DMA_BUF_SYNC_WRITE)
 *            drmIoctl(planefd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &args)
 */
static dri3_screen_info_rec dri3_info = {
    .version = 2,
    .open = NULL,
    .open_client = dri3Open,
    .pixmap_from_fds = dri3PixmapFromFds,
    .fds_from_pixmap = glamor_fds_from_pixmap,
    .fd_from_pixmap = dri3FdFromPixmap,
    .get_formats = dri3GetFormats,
    .get_modifiers = dri3GetModifiers,
    .get_drawable_modifiers = dri3GetDrawableModifiers,
/*    .import_syncobj = NULL, */
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

    scrpriv->hooks.createScreenResources = pScreen->CreateScreenResources;
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
    trace("arcanSetWindowPixmap(%"PRIxPTR", %"PRIxPTR")", (uintptr_t) wnd, (uintptr_t) pixmap);

    arcanPixmapPriv *priv = dixLookupPrivate(
                                             &pixmap->devPrivates,
                                             &pixmapPriv);

/* This might be compose reassigning the pixmap of one window to that of
 * another immediately after arcanCompNewPixmap without passing Realize first -
 * Krita does this for instance */
    if (priv){
        arcanWindowPriv *aWnd = ensureArcanWndPrivate(wnd);
        if (aWnd->shmif && aWnd->shmif->user){
            arcanShmifPriv *contpriv = aWnd->shmif->user;
            contpriv->pixmap = pixmap;
            contpriv->window = wnd; /* should be no-op */
            contpriv->bound = true;
        }
    }

    if (ascr->hooks.setWindowPixmap){
        ascr->screen->SetWindowPixmap = ascr->hooks.setWindowPixmap;
        ascr->hooks.setWindowPixmap(wnd, pixmap);
        ascr->screen->SetWindowPixmap = arcanSetWindowPixmap;
    }
}

struct XWMHints {
    long flags;
    Bool input;
    int initial_state;
    Pixmap icon_pixmap;
    Window icon_window;
    int icon_x, icon_y;
    Pixmap icon_mask;
    XID window_group;
};

static
struct XWMHints *getHintsForWindow(WindowPtr wnd)
{
    if (!wnd->optional)
        return NULL;

    PropertyPtr cp = wnd->optional->userProps;
    while (cp){
        if (cp->propertyName == XA_WM_HINTS)
            return (struct XWMHints*) cp->data;
        cp = cp->next;
    }

    return NULL;
}

static void traceFocus(DeviceIntPtr dev, const char* prefix)
{
    FocusClassPtr focus = dev->focus;
    if (!focus)
        trace("%s : no-Focus",prefix);
    else if (focus->win == PointerRootWin)
        trace("%s : special(PointerRoot)", prefix);
    else if (focus->win == FollowKeyboardWin)
        trace("%s : special(FollowKeyboardWin)", prefix);
    else if (focus->win == NoneWin)
        trace("%s : special(None)", prefix);
    else
        trace("%s : window(%d)", prefix, (int) focus->win->drawable.id);
}

/*
 * Some rules and ICCM nuances to this -
 *
 *       a. if the XWMHints input state is set, the client relies on a message
 *          being sent with the WM_TAKE_FOCUS atom
 *
 *       b. the NET_ACTIVE_WINDOW atom should be set on the root window.
 *
 *       c. SetInputFocus for the internal routing to work.
 *
 */
static
void
updateWindowFocus(WindowPtr wnd, bool focus)
{
    struct XWMHints *hints = getHintsForWindow(wnd);

    DeviceIntPtr kdev = arcanInputPriv.ki->dixdev;
    kdev = GetMaster(kdev, MASTER_KEYBOARD);
    DeviceIntPtr pdev = arcanInputPriv.pi->dixdev;

    if (!focus){
        traceFocus(kdev, "releaseFocus:keyboard:");
        traceFocus(pdev, "releaseFocus:pointer:");

        if (arcanInputPriv.ki->dixdev->focus->win == wnd){
            SetInputFocus(
                         serverClient,
                         kdev,
                         None,
                         RevertToParent,
                         CurrentTime,
                         TRUE
                        );
        }
        return;
    }

    traceFocus(kdev, "setFocus:old_keyboard:");
    traceFocus(pdev, "setFocus:old_pointer:");

    if (hints){
        if (hints->input){
            xEvent xev = (xEvent){
                .u.u.type = ClientMessage,
                .u.u.detail = 32,
                .u.clientMessage.window = wnd->drawable.id,
                .u.clientMessage.u.l.type = wm_atoms.WM_PROTOCOLS,
                .u.clientMessage.u.l.longs0 = wm_atoms.WM_TAKE_FOCUS,
                .u.clientMessage.u.l.longs1 = CurrentTime
            };
            DeliverEventsToWindow(
                PickPointer(serverClient),
                wnd, /* ascr->screen->root, */
                &xev, 1, SubstructureRedirectMask, NullGrab);

            cmdRaiseWindow(wnd->drawable.id);
            trace("sendingTakeFocus(%d)", wnd->drawable.id);
/*            return;  */
        }
    }

    arcanScrPriv *ascr = getArcanScreen(wnd);

    trace("takeRealFocus");
    SetInputFocus(
                  serverClient,
                  kdev,
                  wnd->drawable.id,
                  RevertToParent,
                  CurrentTime,
                  TRUE
                 );
    cmdRaiseWindow(wnd->drawable.id);
    QueueKeyboardEvents(arcanInputPriv.ki->dixdev, EnterNotify, 8);
    traceFocus(kdev, "setFocus, new keyboard:");
    traceFocus(pdev, "setFocus, new mouse:");

    dixChangeWindowProperty(
        serverClient,
        ascr->screen->root,
        wm_atoms._NET_ACTIVE_WINDOW,
        XA_WINDOW,
        32,
        PropModeReplace,
        1,
        &wnd->drawable.id,
        TRUE
    );
}

enum {
    STEP_VBLANK = 1,
    STEP_PRESENT = 2,
    STEP_DELIVERY = 0
};

static void
flushPendingPresent(WindowPtr wnd, int type, uint64_t msc)
{
    if (!arcanConfigPriv.present)
        return;

    struct xa_present_window* pwnd = xa_present_window_priv(wnd);
    if (!pwnd){
        return;
    }

    if (type == 1){
        trace("kind=present_feedback:wnd=%08" PRIx32 ":msc=%"PRIu64, wnd->drawable.id, msc);
        xa_present_buffer_release(pwnd);
    }
    else if (type == 2){
        trace("kind=vblank_feedback:wnd=%08" PRIx32 ":msc=%"PRIu64, wnd->drawable.id, msc);
        xa_present_buffer_release(pwnd); /* not correct, should trigger on #1 and we miss ID to deal with long swap chain */
        xa_present_msc_bump(pwnd, msc);
    }
}

void
arcanFlushRedirectedEvents(int fd, int mask, void *tag)
{
    struct arcan_shmif_cont *C = tag;
    arcanShmifPriv *P = C->user;

    arcan_event ev;
    int rv;

/* Right now just forward input to primary with a suggested origo offset based
 * on the window position to go from relative to absolute. We can also go from
 * io.dst -> XID and explicilty queue to client without going through the
 * normal loop as a way of evading logging. */
    input_lock();

    while ((rv = arcan_shmif_poll(C, &ev)) > 0){
        if (!P->window)
            continue;

        if (ev.category == EVENT_IO){
            TranslateInput(C, &ev, P);
            continue;
        }

        if (ev.category != EVENT_TARGET)
            continue;

        switch (ev.tgt.kind){
        case TARGET_COMMAND_DISPLAYHINT:{
            int hw = ev.tgt.ioevs[0].iv;
            int hh = ev.tgt.ioevs[1].iv;
            updateWindowFocus(P->window, !(ev.tgt.ioevs[2].iv & 4));

            if (hh && hw && (hw != C->w || hh != C->h)){
                trace("segmentResized(%d,%d)", hw, hh);
                cmdReconfigureWindow(0, 0, hw, hh, false, P->window->drawable.id);
/*              flushPendingPresent(P->window); */
            }
        }
        break;

/* Still incomplete as we need changes to upstream Arcan for the last, this
 * blocks -redirect -present -glamor -exec from working completely. */
        case TARGET_COMMAND_STEPFRAME:{
            flushPendingPresent(P->window, ev.tgt.ioevs[1].iv, ev.tgt.ioevs[2].uiv);
        }

        case TARGET_COMMAND_OUTPUTHINT:
/* w/h ranges are interesting as there are IWWM hints to forward */
         break;
/* There is a soft and a kill client option, stick to the soft for now, this is
 * not always available, need to check WM_PROTOCOLS for the appropriate atom.
 * Then mark that the window is intended to be deleted so that we don't get a
 * frame delivered on it until later. */
         break;

/* Treat exit and reset as basically the same for now */
        case TARGET_COMMAND_EXIT:
            trace("segmentDropped");
        case TARGET_COMMAND_RESET:{
            trace("segmentReset:level=%d", ev.tgt.ioevs[0].iv);

            if (windowSupportsProtocol(P->window, wm_atoms.WM_DELETE_WINDOW)){
#ifdef DEBUG
                arcanScrPriv *ascr = getArcanScreen(P->window);
                dumpWindowProperties(ascr->screen->root);
                dumpWindowProperties(P->window);
#endif
                xEvent xev = (xEvent){
                    .u.u.type = ClientMessage,
                    .u.u.detail = 32,
                    .u.clientMessage.window = P->window->drawable.id,
                    .u.clientMessage.u.l.type = wm_atoms.WM_PROTOCOLS,
                    .u.clientMessage.u.l.longs0 = wm_atoms.WM_DELETE_WINDOW,
                    .u.clientMessage.u.l.longs1 = CurrentTime
                };
                DeliverEvents(P->window, &xev, 1, NullWindow);
                P->dying = arcan_timemillis();
           }
/* mimic KillClient */
           else {
           }
        }
        break;
        case TARGET_COMMAND_BCHUNK_IN:{
            if (strcmp(ev.tgt.message, "xkb") == 0){
                loadKeymapFromTextFile(ev.tgt.ioevs[0].iv);
            }
        }
    break;
        case TARGET_COMMAND_ANCHORHINT:
            cmdReconfigureWindow(ev.tgt.ioevs[0].iv,
                                 ev.tgt.ioevs[1].iv, 0, 0, true, P->window->drawable.id);
            trace("anchorHint(%d, %d)", ev.tgt.ioevs[0].iv, ev.tgt.ioevs[1].iv);
        break;
        default:
        break;
        }
    }
    input_unlock();

/* displayhint, request as a configure, possibly set FOCUS */
/* INPUT -> need to inject to parent and make sure the grab is on the window */
}

static void
arcanScreenBlockHandler(ScreenPtr pScreen, void* timeout)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    pScreen->BlockHandler = scrpriv->hooks.screenBlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    pScreen->BlockHandler = arcanScreenBlockHandler;

/* it would be smarter to process this when a client socket has been flushed as
 * that will statistically align better against naturally occurring buffer
 * boundaries, i.e. a per client hook - in addition to the flush from present
 */
    if (scrpriv->redirectBitmap)
    {
        for (size_t i = 0; i < 64; i++){
            struct arcan_shmif_cont *C = scrpriv->pool[i];
            if (!C)
                continue;

            arcanShmifPriv *P = C->user;
            if (!P->bound || !P->window || !P->pixmap)
                continue;

/* dying yet the client hasn't acknowledged it? */
          if (P->dying){
              if (arcan_timemillis() - P->dying > 5000){
                  trace("force-kill:%d", P->window->drawable.id);
                  CloseDownClient(wClient(P->window));
                  P->dying = 0;
              }
              continue;
          }

          if (P->window->unsynched){
              sendWndData(P->window, 0, 0, true, C);
              P->window->unsynched = 0;
          }

/* Support DRI3/Glamor rootless only through PRESENT for the time being.
 * Those segments are signalled out-of-block so we should not arrive here at all. */
          arcanWindowPriv *awnd = dixLookupPrivate(&P->window->devPrivates, &windowPriv);
          if (awnd && awnd->damage){
                RegionPtr region = DamageRegion(awnd->damage);
                if (!RegionNotEmpty(region))
                    continue;

/* If DRI3/glamor is used and comp has an imported BO then it gets more
 * complicated, we can use the mode where subsegments share context with parent
 * so our imports would work for the new segment. In that case we 'just' need
 * to find the BO from the original pixmap and extract that */
                BoxPtr box = RegionExtents(region);
                arcan_shmif_dirty(C, box->x1, box->y1, box->x2, box->y2, 0);
                DamageEmpty(awnd->damage);
           }

           arcan_shmif_signal(C, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
        }
    }
    scrpriv->dirtyBitmap = 0;

    if (scrpriv->damage)
        arcanInternalDamageRedisplay(pScreen);
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

static
Bool
arcanCloseScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    struct arcan_shmif_cont *acon = scrpriv->acon;

    trace("arcanCloseScreen");
    if (!scrpriv)
        return true;

    pScreen->CloseScreen = scrpriv->hooks.closeScreen;
    (*pScreen->CloseScreen)(pScreen);
    pScreen->CloseScreen = arcanCloseScreen;

    scrpriv->acon->user = NULL;
    free(scrpriv);
    screen->driver = NULL;
/* drop shmifext last as that will kill the GL context and thus all the
 * VAOs / VBOs etc. that glamour is using will be defunct */
    arcan_shmifext_drop(acon);

    return true;
}

static void setupRedirect(ScreenPtr screen, arcanScrPriv *ascr)
{
    ascr->clip_mode = ROOT_CLIP_INPUT_ONLY;
    SetRootClip(screen, ascr->clip_mode);
}

Bool
arcanFinishInitScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    resolveAtoms();
    if (arcanConfigPriv.redirect){
        setupRedirect(pScreen, scrpriv);
    }

    trace("arcanFinishInitScreen");
#ifdef RANDR
    if (!arcanRandRInit(pScreen))
        return FALSE;
#endif

    scrpriv->hooks.closeScreen = pScreen->CloseScreen;
    scrpriv->hooks.screenBlockHandler = pScreen->BlockHandler;
    pScreen->CloseScreen = arcanCloseScreen;
    pScreen->BlockHandler = arcanScreenBlockHandler;

/* the present implementation needs are closer to how xwayland does it if you
 * remove frame-callbacks, so ignoring the default present implementation and
 * swapping in a patched version of that. */
    if (arcanConfigPriv.present && arcanConfigPriv.glamor){
        if (!xa_present_init(pScreen)){
            arcanConfigPriv.present = false;
            trace("present_init failed");
        }
    }

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
    scrpriv->hooks.destroyPixmap = screen->pScreen->DestroyPixmap;

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
    screen->pScreen->compNewPixmap = arcanCompNewPixmap;
    screen->pScreen->DestroyPixmap = arcanDestroyPixmap;
#endif

/* hide the root window to indicate that we are 'rootless' */
    arcan_shmif_mousestate_setup(scrpriv->acon, false, NULL);
    if (arcanConfigPriv.redirect){
        scrpriv->defaultRootless = true;
        ScreenSaverTime = 0;
        ScreenSaverInterval = 0;
        defaultScreenSaverTime = 0;
        defaultScreenSaverInterval = 0;
        ARCAN_ENQUEUE(scrpriv->acon, &(struct arcan_event){
             .category = EVENT_EXTERNAL,
             .ext.kind = ARCAN_EVENT(VIEWPORT),
             .ext.viewport.invisible = true
        });
    }

    return TRUE;
}

void
arcanScreenFini(KdScreenInfo * screen)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    if (!con)
        return;

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
