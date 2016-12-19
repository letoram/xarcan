/*
 * Copyright © 2004 Keith Packard
 * Copyright © 2016 Bjorn Stahl
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
 * See README-Arcan.md for details on status and todo.
 */
#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif

#ifdef GLAMOR
#define MESA_EGL_NO_X11_HEADERS
#include <gbm.h>
#include "glamor.h"
#include "glamor_context.h"
#include "glamor_egl.h"
#include "dri3.h"
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include <X11/keysym.h>

/*
 * from Xwin
 */
#include <opaque.h>
#define XSERV_t
#define TRANS_SERVER
#include <X11/Xtrans/Xtrans.h>

arcanInput arcanInputPriv;
arcanConfig arcanConfigPriv;
int arcanGlamor;

static uint8_t code_tbl[512];
static struct arcan_shmif_initial* arcan_init;

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
    scrpriv->acon->hints = SHMIF_RHINT_SUBREGION | SHMIF_RHINT_IGNORE_ALPHA;
    trace("arcanScreenInitialize");

    if (!screen->width || !screen->height) {
        screen->width = scrpriv->acon->w;
        screen->height = scrpriv->acon->h;
/* we still need to synch the changes in display flags */
        arcan_shmif_resize(scrpriv->acon, screen->width, screen->height);
    }
    else {
        if (!arcan_shmif_resize(scrpriv->acon, screen->width, screen->height)){
                screen->width = scrpriv->acon->w;
                screen->height = scrpriv->acon->h;
        }
    }

/* default guess, we cache this between screen init/deinit */
        if (!arcan_init)
            arcan_shmif_initial(scrpriv->acon, &arcan_init);

        if (arcan_init->density > 0){
            screen->width_mm = (float)screen->width / (0.1 * arcan_init->density);
            screen->height_mm = (float)screen->height / (0.1 * arcan_init->density);
        }

#define Mask(o,l)   (((1 << l) - 1) << o)
    screen->rate = 60;
    screen->fb.depth = 24;
    screen->fb.bitsPerPixel = 32;
    screen->fb.redMask = SHMIF_RGBA(0xff, 0x00, 0x00, 0x00);
    screen->fb.greenMask = SHMIF_RGBA(0x00, 0xff, 0x00, 0x00);
    screen->fb.blueMask = SHMIF_RGBA(0x00, 0x00, 0xff, 0x00);
    screen->fb.visuals = (1 << TrueColor) | (1 << DirectColor);
    scrpriv->randr = screen->randr;

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
        KdEnqueueKeyboardEvent(arcanInputPriv.ki, ev->input.translated.scancode,
            !ev->input.translated.active);
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

/* release on focus loss */
    if (fl & 4){
        for (size_t i = 0; i < sizeof(code_tbl) / sizeof(code_tbl[0]); i++){
            if (code_tbl[i]){
                KdEnqueueKeyboardEvent(arcanInputPriv.ki, i, 1);
                code_tbl[i] = 0;
            }
        }
    }

    if (arcanConfigPriv.no_dynamic_resize)
        return;

    if (w >= 640 && h >= 480 && (con->w != w || con->h != h) && !(fl & 1)){
        RRScreenSetSizeRange(apriv->screen,
            640, 480, PP_SHMPAGE_MAXW, PP_SHMPAGE_MAXH);
        arcanRandRScreenResize(apriv->screen,
            w, h,
            ppcm > 0 ? (float)w / (0.1 * ppcm) : 0,
            ppcm > 0 ? (float)h / (0.1 * ppcm) : 0
        );
        return;
    }
/* NOTE:
 * on focus lost or window hidden, should we just stop synching and
 * waiting for it to return?
 */

/*
 * we do want to inject 'modifiers lost' if focus is lost.
 */
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
            case TARGET_COMMAND_RESET:
            break;
            case TARGET_COMMAND_DISPLAYHINT:
                arcanDisplayHint(con,
                    ev.tgt.ioevs[0].iv, ev.tgt.ioevs[1].iv,
                    ev.tgt.ioevs[2].iv, ev.tgt.ioevs[3].iv, ev.tgt.ioevs[4].iv);
            break;
            case TARGET_COMMAND_OUTPUTHINT:
            break;
            case TARGET_COMMAND_NEWSEGMENT:
/* grab clipboard and match with X clipboard integration here, this can
 * be made similar to how it's working for Xwin and others */
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

/* primary connection is a allocated once and then retained for the length of
 * the process */
    if (con->user){
        fprintf(stderr, "multiple screen support still missing\n");
        abort();
    }

    scrpriv->acon = con;
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

    if (!RegionNotEmpty(region) || scrpriv->acon->addr->vready)
        return;

    trace("arcanInternalDamageRedisplay");

/*
 * We don't use fine-grained dirty regions really, the data gathered gave
 * quite few benefits as cases with many dirty regions quickly exceeded the
 * magic ratio where subtex- update vs full texture update tipped in favor
 * of the latter.
 */
    box = RegionExtents(region);
    scrpriv->acon->dirty.x1 = box->x1;
    scrpriv->acon->dirty.x2 = box->x2;
    scrpriv->acon->dirty.y1 = box->y1;
    scrpriv->acon->dirty.y2 = box->y2;

/*
 * This works, surprisingly, I'd assume we should need double buffered
 * visuals to scrpriv->tex but apparently not.
 */
#ifdef GLAMOR
    if (scrpriv->in_glamor){
        trace("arcanInternalDamageRedisplay:signal-glamor");
        arcan_shmifext_signal(scrpriv->acon, 0,
            SHMIF_SIGVID | SHMIF_SIGBLK_NONE, scrpriv->tex);
    }
    else{
#endif
        arcan_shmif_signal(scrpriv->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE );
        trace("arcanInternalDamageRedisplay:signal");
    }
    DamageEmpty(scrpriv->damage);
}

static Bool arcanSetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pPixmap = NULL;
    trace("arcanSetInternalDamage");

    scrpriv->damage = DamageCreate((DamageReportFunc) 0,
                                   (DamageDestroyFunc) 0,
                                   DamageReportBoundingBox,
                                   TRUE, pScreen, pScreen);

    pPixmap = (*pScreen->GetScreenPixmap) (pScreen);

    DamageRegister(&pPixmap->drawable, scrpriv->damage);
    DamageSetReportAfterOp(scrpriv->damage, TRUE);

    return TRUE;
}

static void arcanUnsetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    trace("arcanUnsetInternalDamage");

    DamageDestroy(scrpriv->damage);
    scrpriv->damage = NULL;
}

static
int arcanSetPixmapVisitWindow(WindowPtr window, void *data)
{
    ScreenPtr screen = window->drawable.pScreen;
    trace("arcanSetPixmapVisitWindow");
    if (screen->GetWindowPixmap(window) == data){
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }
    return WT_DONTWALKCHILDREN;
}

static
Bool arcanGlamorCreateScreenResources(ScreenPtr pScreen)
{
#ifdef GLAMOR
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr oldpix, newpix;

    trace("arcanGlamorCreateScreenResources");
    scrpriv->CreateScreenResources(pScreen);


    oldpix = pScreen->GetScreenPixmap(pScreen);
/*
    pScreen->DestroyPixmap(oldpix);
 */

    newpix = pScreen->CreatePixmap(pScreen,
                                   pScreen->width,
                                   pScreen->height,
                                   pScreen->rootDepth,
                                   CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

    if (newpix){
        if (pScreen->root && pScreen->SetWindowPixmap)
            TraverseTree(pScreen->root, arcanSetPixmapVisitWindow, oldpix);

        pScreen->SetScreenPixmap(newpix);
        glamor_set_screen_pixmap(newpix, NULL);
        scrpriv->tex = glamor_get_pixmap_texture(newpix);
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
    arcanPriv *priv = screen->card->driver;
    trace("arcanUnmapFramebuffer");
    free(priv->base);
    priv->base = NULL;
    return TRUE;
}

static int
ArcanInit(void)
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

/* we will do dirt- region updates rather than full swaps, and alpha
 * channel will be set to 00 so ignore that one */
    arcan_shmif_setprimary(SHMIF_INPUT, con);

    return 1;
}

static void
ArcanEnable(void)
{
    trace("ArcanEnable");
}

static Bool
ArcanSpecialKey(KeySym sym)
{
    trace("ArcanSpecialKey");
    return FALSE;
}

static void
ArcanDisable(void)
{
    trace("ArcanDisable");
}

static void
ArcanFini(void)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    trace("ArcanFini");
    if (con){
        arcan_shmif_drop(con);
    }
}

KdOsFuncs arcanOsFuncs = {
    .Init = ArcanInit,
    .Enable = ArcanEnable,
    .SpecialKey = ArcanSpecialKey,
    .Disable = ArcanDisable,
    .Fini = ArcanFini
};
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

    if (width == screen->width && height == screen->height)
        return FALSE;

    trace("ArcanRandRScreenResize");
    size.width = width;
    size.height = height;
    size.mmWidth = mmWidth;
    size.mmHeight = mmHeight;

    if (arcanRandRSetConfig(pScreen, screen->randr, 0, &size)){
        RROutputPtr output = RRFirstOutput(pScreen);
        if (!output)
            return FALSE;
        RROutputSetModes(output, NULL, 0, 0);
    }

    return FALSE;
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
    newwidth = pSize->height;
    newheight = pSize->width;

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
    arcan_shmif_resize(scrpriv->acon, newwidth, newheight);
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

    if (!arcanSetInternalDamage(screen->pScreen))
        goto bail4;


    /* set the subpixel order */

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
#endif
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

int glamor_egl_dri3_fd_name_from_tex(ScreenPtr pScreen,
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

/* size? there is a glamor_gbm_bo_from_pixmap() but these do not
 * seem to provide the format needed to go on. */
    if (arcan_shmifext_gltex_handle(scrpriv->acon, 0,
                                    tex, &fd, &dstr, &fmt)){
        *stride = dstr;
        return fd;
    }

    *stride = 0;
    *size = 0;
    return -1;
}

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

/*
 * Only support GBM
 */
static int dri3FdFromPixmap(ScreenPtr pScreen, PixmapPtr pixmap,
                            CARD16 *stride, CARD32 *size)
{
    trace("ArcanDRI3FdFromPixmap");
/*
 *  1. get the pixmap structure
 *  2. extract BO
 *  gbm_bo_get_stride,
 *  pixmap->drawable.width * *stride,
 *  return gbm_bo_get_fd(bo);
 */
    return -1;
}

static PixmapPtr dri3PixmapFromFd(ScreenPtr pScreen, int fd,
                            CARD16 w, CARD16 h, CARD16 stride,
                            CARD8 depth, CARD8 bpp)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pixmap;
    struct gbm_bo *bo;
    struct gbm_import_fd_data data = {
        .fd = fd, .width = w, .height = h, .stride = stride };

    trace("ArcanDRI3PixmapFromFD(%d, %d, %d, %d, %d)",
                                 (int)w, (int)h,
                                 (int)stride, (int)depth,
                                 (int)bpp );

    if (!w || !h || bpp != BitsPerPixel(depth) ||
        stride < w * h * (bpp / 8)){
        trace("ArcanDRI3PixmapFromFD()::Bad arguments");
        return NULL;
    }

/*  data.format = gbm_format_for_depth(depth); */
    if (bo == NULL){
        trace("ArcanDRI3PixmapFromFD()::Couldn't import BO");
        return NULL;
    }

/* Unfortunately shmifext- don't expose the buffer-import setup yet,
 * waiting for the whole GBM v Streams to sort itself out, so just
 * replicate that code once more. */
    pixmap = glamor_create_pixmap(pScreen,
                                  gbm_bo_get_width(bo),
                                  gbm_bo_get_height(bo),
                                  depth,
                                  GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
    if (pixmap == NULL) {
        return NULL;
    }

    arcan_shmifext_make_current(scrpriv->acon);

/* Could probably do more sneaky things here to forward this buffer
 * to arcan as its own window/source to cut down on the dedicated
 * fullscreen stuff.
    xwl_pixmap->bo = bo;
    xwl_pixmap->buffer = NULL;
    xwl_pixmap->image = eglCreateImageKHR(xwl_screen->egl_display,
                                          xwl_screen->egl_context,
                                          EGL_NATIVE_PIXMAP_KHR,
                                          xwl_pixmap->bo, NULL);

    glGenTextures(1, &xwl_pixmap->texture);
    glBindTexture(GL_TEXTURE_2D, xwl_pixmap->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, xwl_pixmap->image);
    glBindTexture(GL_TEXTURE_2D, 0);

    xwl_pixmap_set_private(pixmap, xwl_pixmap);

    glamor_set_pixmap_texture(pixmap, xwl_pixmap->texture);
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
 */
    return pixmap;
}

static int dri3Open(ClientPtr client,
                    ScreenPtr pScreen,
                    RRProviderPtr provider,
                    int *pfd)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    int fd = arcan_shmifext_dev(scrpriv->acon);
    trace("ArcanDri3Open(%d)", fd);
    if (-1 != fd){
        *pfd = dup(fd);
        return Success;
    }
    return BadAlloc;
}

static dri3_screen_info_rec dri3_info = {
    .version = 1,
    .open_client = dri3Open,
    .pixmap_from_fd = dri3PixmapFromFd,
    .fd_from_pixmap = dri3FdFromPixmap
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
    defs.depth = 0;
    defs.alpha = 0;
    defs.major = GLAMOR_GL_CORE_VER_MAJOR;
    defs.minor = GLAMOR_GL_CORE_VER_MINOR;
    defs.mask  = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
    trace("arcanGlamorInit");

    defs.builtin_fbo = false;
    if (SHMIFEXT_OK != arcan_shmifext_setup(scrpriv->acon, defs)){
        ErrorF("xarcan/glamor::init() - EGL context failed, lowering version");
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
 *   though is it still relevant / needed?
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

    if (!dri3_screen_init(pScreen, &dri3_info)){
        ErrorF("arcanGlamorInit() - failed to set DRI3");
        arcan_shmifext_drop(scrpriv->acon);
        goto bail;
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
 * for the next one. The other option is to pop a CLOCKREQ and accept the slight
 * latency when synch does not align.
 */
//    trace("arcanScreenBlockHandler(%lld)",(long long) currentTime.milliseconds);
    pScreen->BlockHandler = scrpriv->BlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

    if (scrpriv->damage)
        arcanInternalDamageRedisplay(pScreen);
    else
        arcan_shmif_signal(scrpriv->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
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
    arcanCloseScreen(pScreen);
    return TRUE;
}

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

    return TRUE;
}

Bool
arcanCreateResources(ScreenPtr pScreen)
{
    trace("arcanCreateResources");
    return arcanSetInternalDamage(pScreen);
}

void
arcanPreserve(KdCardInfo * card)
{
    trace("arcanPreserve");
}

Bool
arcanEnable(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    trace("arcanEnable");
    arcan_shmif_enqueue(scrpriv->acon, &(arcan_event){
        .ext.kind = ARCAN_EVENT(VIEWPORT),
        .ext.viewport = {
            .invisible = false
        }
    });

    return TRUE;
}

Bool
arcanDPMS(ScreenPtr pScreen, int mode)
{
    trace("arcanDPMS");
    return TRUE;
}

void
arcanDisable(ScreenPtr pScreen)
{
    trace("arcanDisable");
/*
 * arcan_shmif_enqueue(scrpriv->acon, &(arcan_event){
        .ext.kind = ARCAN_EVENT(VIEWPORT),
        .ext.viewport = {
            .invisible = false
        }
    });
 */
}

void
arcanRestore(KdCardInfo * card)
{
/* NOOP */
    trace("arcanRestore");
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

    arcanUnsetInternalDamage(pScreen);
    arcan_shmifext_drop(scrpriv->acon);

    scrpriv->acon->user = NULL;
    pScreen->CloseScreen = NULL;
    free(scrpriv);
    screen->driver = NULL;
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
