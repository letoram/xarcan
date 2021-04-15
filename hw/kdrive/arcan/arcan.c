/*
 * Copyright © 2004 Keith Packard
 * Copyright © 2016-2017 Bjorn Stahl
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

/*
 * TODO
 *
 * - [ ] Re-add DRI3 buffer hooks
 * - [ ] Rootless window buffer redirection
 * - [ ] Xace buffer interception
 * - [ ] Accelerated cursor mapping
 * - [ ] Track / map window creation/positioning/mapping/selection/ordering
 *   - [ ] forward over
 *   - [ ] map proxy windows back
 * - [ ] Single-Buffer
 * - [ ] PRESENT
 * - [ ] Shmif-server thread, map to windows and event loop
 * - [ ] Exec / connection inherit setup (needs tampering with xlib/xcb preload)
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include <X11/keysym.h>

#ifdef GLAMOR
#define MESA_EGL_NO_X11_HEADERS
#include <gbm.h>
#include "glamor.h"
#include "glamor_context.h"
#include "glamor_egl.h"
#include "dri3.h"
#include <drm_fourcc.h>
#endif

/*
 * from Xwin
 */
#include <opaque.h>
#define XSERV_t
#define TRANS_SERVER
#include <X11/Xtrans/Xtrans.h>
#include <present.h>

arcanInput arcanInputPriv;
arcanConfig arcanConfigPriv;
int arcanGlamor;

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
static DevPrivateKeyRec pixmap_private_key;

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

/*
 * likely that these calculations are incorrect, need BE machines to
 * test between both setArcan and setGlamor versions, stick with the
 * setGlamor version for now.
 */
static void setArcanMask(KdScreenInfo* screen)
{
    screen->rate = 60;
    screen->fb.depth = 24;
    screen->fb.bitsPerPixel = 32;
    screen->fb.visuals = (1 << TrueColor) | (1 << DirectColor);
    screen->fb.redMask = SHMIF_RGBA(0xff, 0x00, 0x00, 0x00);
    screen->fb.greenMask = SHMIF_RGBA(0x00, 0xff, 0x00, 0x00);
    screen->fb.blueMask = SHMIF_RGBA(0x00, 0x00, 0xff, 0x00);
 }

static void setGlamorMask(KdScreenInfo* screen)
{
    int bpc, green_bpc;
    screen->rate = 60;
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

Bool
arcanScreenInitialize(KdScreenInfo * screen, arcanScrPriv * scrpriv)
{
    scrpriv->acon->hints =
/*       SHMIF_RHINT_SUBREGION | */
			 SHMIF_RHINT_IGNORE_ALPHA |SHMIF_RHINT_VSIGNAL_EV;
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
    if (1 || arcanGlamor)
        setGlamorMask(screen);
    else
        setArcanMask(screen);
    return arcanMapFramebuffer(screen);
}

static void
TranslateInput(struct arcan_shmif_cont* con, arcan_ioevent* ev, int* x, int* y)
{
    if (ev->devkind == EVENT_IDEVKIND_MOUSE){
        int flags = arcanInputPriv.pi->buttonState;
        if (ev->datatype == EVENT_IDATATYPE_ANALOG){
/* buffer the relatives and push them in once fell swoop */
            if (ev->input.analog.gotrel){
                switch (ev->input.analog.nvalues){
                case 1:
                case 2:
                case 3:
                    if (ev->subid == 0)
                        *x += ev->input.analog.axisval[0];
                    else if (ev->subid == 1)
                        *y += ev->input.analog.axisval[0];
                break;
                case 4:
                    *x += ev->input.analog.axisval[0];
                    *y += ev->input.analog.axisval[2];
                break;
                }
            }
            else {
                static int ox, oy;
                bool dirty = false;
                flags |= KD_POINTER_DESKTOP;

                switch (ev->input.analog.nvalues){
                case 1: case 2:
                    if (ev->subid == 0){
                        dirty |= ev->input.analog.axisval[0] != ox;
                        ox = ev->input.analog.axisval[0];
                    }
                    else if (ev->subid == 1){
                        dirty |= ev->input.analog.axisval[0] != oy;
                        oy = ev->input.analog.axisval[0];
                    }
                break;
                case 3: case 4:
                    dirty |= ev->input.analog.axisval[0] != ox;
                    dirty |= ev->input.analog.axisval[2] != oy;
                    ox = ev->input.analog.axisval[0];
                    oy = ev->input.analog.axisval[2];
                break;
                }

                if (dirty)
                    KdEnqueuePointerEvent(arcanInputPriv.pi, flags, ox, oy, 0);
            }
        }
        else {
            int ind = -1;
            flags |= KD_MOUSE_DELTA;
            switch (ev->subid){
            case MBTN_LEFT_IND: ind = KD_BUTTON_1; break;
            case MBTN_RIGHT_IND: ind = KD_BUTTON_3; break;
            case MBTN_MIDDLE_IND: ind = KD_BUTTON_2; break;
            case MBTN_WHEEL_UP_IND: ind = KD_BUTTON_4; break;
            case MBTN_WHEEL_DOWN_IND: ind = KD_BUTTON_5; break;
            default:
                return;
            }
            if (ev->input.digital.active)
                flags |= ind;
            else
                flags = flags & (~ind);
            if (*x != 0 || *y != 0){
                KdEnqueuePointerEvent(arcanInputPriv.pi, flags, *x, *y, 0);
                *x = 0;
                *y = 0;
            }
            KdEnqueuePointerEvent(arcanInputPriv.pi, flags, 0, 0, 0);
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
/* NOTE:
 * on focus lost or window hidden, should we just stop synching and
 * waiting for it to return?
 */
}

static
void
arcanSignalShm(struct arcan_shmif_cont* con, bool dirty)
{
  arcanScrPriv *scrpriv = con->user;
	if (!scrpriv->dirty && !dirty)
		return;

	arcan_shmif_signal(con, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
 	con->dirty.x1 = con->w;
	con->dirty.y1 = con->h;
	con->dirty.y2 = 0;
	con->dirty.x2 = 0;
	scrpriv->dirty = false;
}

void
arcanFlushEvents(int fd, void* tag)
{
    int mx = 0, my = 0;
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    struct arcan_event ev;
    if (!con || !con->user)
        return;

    while (arcan_shmif_poll(con, &ev) > 0){
        if (ev.category == EVENT_IO){
            TranslateInput(con, &(ev.io), &mx, &my);
        }
        else if (ev.category != EVENT_TARGET)
            continue;
        else
            switch (ev.tgt.kind){
            case TARGET_COMMAND_STEPFRAME:
						    arcanSignalShm(con, false);
            break;
            case TARGET_COMMAND_RESET:
                switch(ev.tgt.ioevs[0].iv){
/* not much 'state' that we can extract or track from the Xorg internals,
 * possibly input related and likely not worth it */
                    case 0:
                    case 1:
                    break;
/* for recovery / hard-migrate, we don't really need to do anything yet */
                    case 2:
                    break;
/* swap-out monitored FD */
                    case 3:
                       InputThreadUnregisterDev(con->epipe);
                       InputThreadRegisterDev(con->epipe, (void*) arcanFlushEvents, con);
											 arcanSignalShm(con, true);
                    break;
                }
            break;
            case TARGET_COMMAND_DISPLAYHINT:
                arcanDisplayHint(con,
                    ev.tgt.ioevs[0].iv, ev.tgt.ioevs[1].iv,
                    ev.tgt.ioevs[2].iv, ev.tgt.ioevs[3].iv, ev.tgt.ioevs[4].iv);
            break;
            case TARGET_COMMAND_OUTPUTHINT:
            break;
            case TARGET_COMMAND_NEWSEGMENT:
/* Only thing we care about here is if a new output segment is pushed, that
 * would indicate that we have received a buffer to 'provide' if a client
 * attempts to read or capture anything from the root display. Clipboard is
 * used with the aclip tool, so no need here - and mouse cursor registration is
 * performed much earlier */
            break;
            case TARGET_COMMAND_EXIT:
                CloseWellKnownConnections();
                OsCleanup(1);
                exit(1);
            break;
        default:
        break;
        }
    }

		arcanSignalShm(con, false);

/* aggregate mouse input events unless it's clicks,
 * where we have to flush for the values to register correctly */
    if (mx != 0 || my != 0){
        KdEnqueuePointerEvent(arcanInputPriv.pi,
            KD_MOUSE_DELTA | arcanInputPriv.pi->buttonState,
            mx, my, 0
        );
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

    if (!dixRegisterPrivateKey(&pixmap_private_key, PRIVATE_PIXMAP, 0)){
        return FALSE;
    }

/* primary connection is a allocated once and then retained for the length of
 * the process */
    if (con->user){
        fprintf(stderr, "multiple screen support still missing\n");
        abort();
    }

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
    RegionPtr region;
    BoxPtr box;

    if (!pScreen)
        return;

    region = DamageRegion(scrpriv->damage);

    if (!RegionNotEmpty(region))
			return;

/*
 * We don't use fine-grained dirty regions really, the data gathered gave
 * quite few benefits as cases with many dirty regions quickly exceeded the
 * magic ratio where subtex- update vs full texture update tipped in favor
 * of the latter.
 */
    box = RegionExtents(region);
    if (box->x1 < scrpriv->acon->dirty.x1)
        scrpriv->acon->dirty.x1 = box->x1;

    if (box->x2 > scrpriv->acon->dirty.x2)
        scrpriv->acon->dirty.x2 = box->x2;

    if (box->y1 < scrpriv->acon->dirty.y1)
        scrpriv->acon->dirty.y1 = box->y1;

    if (box->y2 > scrpriv->acon->dirty.y2)
        scrpriv->acon->dirty.y2 = box->y2;

#ifdef GLAMOR
    if (scrpriv->in_glamor){
        int fd = -1;
        size_t stride = 0;
        int fourcc = 0;

/* does glamor buffer somewhere internally? this do not seem to be
 * plagued by tearing or locking issues, even though it likely should.
 * maybe it's just because the tested display and simulated rate match?
 * in that case, the delivery- approach used for non-glamor should work */
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

/* This should be changed into the planes + modifiers + ... format */
        arcan_shmif_signalhandle(scrpriv->acon,
                                 SHMIF_SIGVID | SHMIF_SIGBLK_NONE,
                                 fd, stride, fourcc);
        if (-1 != scrpriv->pending_fd){
            close(scrpriv->pending_fd);
        }
        scrpriv->pending_fd = fd;
    }
		else
#endif
		{
/* use frame delivery event to indicate next time we should signal */
  		DamageEmpty(scrpriv->damage);
	    scrpriv->dirty = true;
		}
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
    else if (depth == 32)
        return GBM_FORMAT_ARGB8888;
    else
        ErrorF("unhandled gbm depth: %d\n", depth);
    return 0;
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
/*
static
int drmFmt(int depth)
{
    switch (depth){
    case 16: return DRM_FORMAT_RGB565;
    case 24: return DRM_FORMAT_XRGB8888;
    case 32: return DRM_FORMAT_ARGB8888;
    default:
        return -1;
    break;
    }
}*/

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

    dixSetPrivate(&pixmap->devPrivates, &pixmap_private_key, ext_pixmap);
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

    oldpix = pScreen->GetScreenPixmap(pScreen);
    if (-1 == arcan_shmifext_dev(scrpriv->acon, &dev, false)){
        trace("ArcanDRI3PixmapFromFD()::Couldn't get device handle");
        return false;
    }

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
    if (!newpix){
        newpix = pScreen->CreatePixmap(pScreen,
                                       pScreen->width,
                                       pScreen->height,
                                       pScreen->rootDepth,
                                       CREATE_PIXMAP_USAGE_BACKING_PIXMAP);
        scrpriv->bo = NULL;
        scrpriv->tex = (newpix ? glamor_get_pixmap_texture(newpix) : -1);
    }

    if (newpix){
        trace("SetScreenPixmap(new)");
        pScreen->SetScreenPixmap(newpix);
/*        glamor_set_screen_pixmap(newpix, NULL); */

      if (pScreen->root && pScreen->SetWindowPixmap)
            TraverseTree(pScreen->root, ArcanSetPixmapVisitWindow, oldpix);

        SetRootClip(pScreen, ROOT_CLIP_FULL);
    }

    return TRUE;
#endif
   return FALSE;
}

void *
arcanWindowLinear(ScreenPtr pScreen,
                 CARD32 row,
                 CARD32 offset, int mode, CARD32 *size, void *closure)
{
    KdScreenPriv(pScreen);
    arcanPriv *priv = pScreenPriv->card->driver;

    if (!pScreenPriv->enabled)
        return 0;
    *size = priv->bytes_per_line;
    return priv->base + row * priv->bytes_per_line;
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

/* try and request a mouse segment for accelerated mouse (unless disabled)
 * or at least indicate that we do software cursor rendering */
    if (arcanConfigPriv.accel_cursor){
        arcan_shmif_enqueue(con, &(struct arcan_event){
            .ext.kind = ARCAN_EVENT(SEGREQ),
            .ext.segreq.width = 32,
            .ext.segreq.height = 32,
            .ext.segreq.kind = SEGID_CURSOR,
            .ext.segreq.id = 0xbad1dea
        });

/*
 * wd->cursor = arcan_shmif_acquire(&wd->mcont, NULL, SEGID_CURSOR, 0);
        printf("parent REJECTED cursor\n");
        wd->cursor_reject = true;
 */
    }
        else {
            arcan_shmif_enqueue(con, &(arcan_event){
        .category = EVENT_EXTERNAL,
        .ext.kind = ARCAN_EVENT(CURSORHINT),
        .ext.message.data = "hidden"
        });
        }

/* we will do dirty- region updates rather than full swaps, and alpha
 * channel will be set to 00 so ignore that one */
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
            RRModePtr mode = arcan_cvt(width, height, 60.0 / 1000.0, 0, 0);

        if (!scrpriv->randrOutput){
          trace("arcanRandRInit(No output)");
            return FALSE;
        }

/* need something else here, like Crtcnotify */
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
    if (arcanGlamor){
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
                                             &pixmap_private_key);
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
                         GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_RENDERING);
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
	*num_modifiers = 0;
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
arcanScreenBlockHandler(ScreenPtr pScreen, void* timeout)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

/*
 * This one is rather unfortunate as it seems to be called 'whenever' and we
 * don't know the synch- state of the contents in the buffer. At the same
 * time, it doesn't trigger when idle- so we can't just skip synching and wait
 * for the next one. We can also set the context to be in event-frame delivery
 * mode and use the STEPFRAME event as a trigger to signal-if-dirty
 */
//    trace("arcanScreenBlockHandler(%lld)",(long long) currentTime.milliseconds);
    pScreen->BlockHandler = scrpriv->BlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

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
    trace("present:vblank abort");
}

static int arcanPresentQueueVblank(RRCrtcPtr crtc, uint64_t evid, uint64_t msc)
{
    trace("present:queue vblank (wait for vready)");
    return Success;
}

static void
arcanPresentFlush(WindowPtr window)
{
    trace("present:flush");
#ifdef GLAMOR
/*
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);

    if (ms->drmmode.glamor)
        glamor_block_handler(screen);
 */
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

static Bool
arcanPresentCheckFlip(RRCrtcPtr crtc,
                      WindowPtr window,
                                            PixmapPtr pixmap,
                                            Bool sync_flip)
{
    trace("present:check flip");
    return true;
}

static present_screen_info_rec arcan_present_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = arcanPresentGetCrtc,
    .get_ust_msc = arcanPresentGetUstMsc,
    .queue_vblank = arcanPresentQueueVblank,
    .abort_vblank = arcanPresentAbortVblank,
    .flush = arcanPresentFlush,
    .capabilities = PresentCapabilityAsync,
    .check_flip = arcanPresentCheckFlip,
    .flip = arcanPresentFlip,
    .unflip = arcanPresentUnflip
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

    present_screen_init(pScreen, &arcan_present_info);
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
/* we need a provision to signal this, can probably use the input-
 * event used by remoting */
    trace("arcanKeyboardLeds(%d)", leds);
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

    trace("arcanCloseScreen");
    if (!scrpriv)
        return;

/*  ASAN reports UAF if we manually Unset @ Close
 *  arcanUnsetInternalDamage(pScreen);
 */
    arcan_shmifext_drop(scrpriv->acon);

    scrpriv->acon->user = NULL;
    pScreen->CloseScreen = scrpriv->CloseHandler;
    free(scrpriv);
    screen->driver = NULL;
    (*pScreen->CloseScreen)(pScreen);
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

miPointerScreenFuncRec ArcanPointerScreenFuncs = {
/* CursorOffScreen
 * CrossScreen
 * WarpCursor */
};

miPointerSpriteFuncRec ArcanPointerSpriteFuncs = {
/*
 * Realize
 * Unrealize
 * SetCursor
 * MoveCursor
 * Initialize
 * Cleanup
 */
};

/*
static DevPrivateKeyRec cursor_private_key;
static Bool
static struct arcan_shmif_cont cursor;
arcanCursorInit(ScreenPtr screen)
{
    if (!arcanConfigPriv.accel_cursor)
        return FALSE;

    if (!dixRegisterPrivateKey(&cursor_private_key, PRIVATE_CURSOR_BITS, 0))
        return FALSE;

    miPointerInitialize(screen,
                        &ArcanPointerSpriteFuncs,
                        &ArcanPointerScreenFuncs, FALSE);

    return TRUE;
}
 */

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

KdPointerDriver arcanPointerDriver = {
    "arcan",
    MouseInit,
    MouseEnable,
    MouseDisable,
    MouseFini,
    NULL
};
