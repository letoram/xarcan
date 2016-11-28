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

Bool
arcanInitialize(KdCardInfo * card, arcanPriv * priv)
{
    priv->base = 0;
    priv->bytes_per_line = 0;
    return TRUE;
}

Bool
arcanCardInit(KdCardInfo * card)
{
    arcanPriv *priv;

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
    static struct arcan_shmif_initial* init;
    if (!screen->width || !screen->height) {
        screen->width = scrpriv->acon->w;
        screen->height = scrpriv->acon->h;
    }
    else {
        if (!arcan_shmif_resize(scrpriv->acon, screen->width, screen->height)){
                screen->width = scrpriv->acon->w;
                screen->height = scrpriv->acon->h;
        }
    }

/* default guess, we cache this between screen init/deinit */
        if (!init)
            arcan_shmif_initial(scrpriv->acon, &init);

        if (init->density > 0){
            screen->width_mm = (float)screen->width / (0.1 * init->density);
            screen->height_mm = (float)screen->height / (0.1 * init->density);
        }

#define Mask(o,l)   (((1 << l) - 1) << o)
    screen->rate = 60;
    screen->fb.depth = 32;
    screen->fb.bitsPerPixel = 32;
    screen->fb.redMask = SHMIF_RGBA(0xff, 0x00, 0x00, 0x00);
    screen->fb.greenMask = SHMIF_RGBA(0x00, 0xff, 0x00, 0x00);
    screen->fb.blueMask = SHMIF_RGBA(0x00, 0x00, 0xff, 0x00);
    screen->fb.visuals = (1 << TrueColor);

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
        KdEnqueueKeyboardEvent(arcanInputPriv.ki, ev->input.translated.scancode,
            !ev->input.translated.active);
  }
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
/* Check if dynamic- resize is accepted, and if so, reconfigure randr etc.
 * to take the new dimensions into account. */
            break;
            case TARGET_COMMAND_OUTPUTHINT:
/* send / reset modifiers when we lose focus */
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
    RegionPtr region = DamageRegion(scrpriv->damage);
    BoxPtr box;

    if (!RegionNotEmpty(region))
        return;

    box = RegionExtents(region);
    scrpriv->acon->dirty.x1 = box->x1;
    scrpriv->acon->dirty.x2 = box->x2;
    scrpriv->acon->dirty.y1 = box->y1;
    scrpriv->acon->dirty.y2 = box->y2;

    arcan_shmif_signal(scrpriv->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE );
		DamageEmpty(scrpriv->damage);
}

static Bool arcanSetInternalDamage(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    PixmapPtr pPixmap = NULL;

    scrpriv->damage = DamageCreate((DamageReportFunc) 0,
                                   (DamageDestroyFunc) 0,
                                   DamageReportNone, TRUE, pScreen, pScreen);

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

    DamageDestroy(scrpriv->damage);
    scrpriv->damage = NULL;
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

    KdComputePointerMatrix(&m, scrpriv->randr, screen->width, screen->height);
    KdSetPointerMatrix(&m);

    screen->fb.byteStride = scrpriv->acon->stride;
    screen->fb.pixelStride = scrpriv->acon->pitch;
    screen->fb.frameBuffer = (CARD8 *) (scrpriv->acon->vidp);

    return TRUE;
}

void
arcanSetScreenSizes(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;
    float ind = ARCAN_SHMPAGE_DEFAULT_PPCM * 0.1;
    struct arcan_shmif_initial* init;
    int inw, inh;

/* default guess */
    arcan_shmif_initial(scrpriv->acon, &init);
    inw = scrpriv->acon->w;
    inh = scrpriv->acon->h;

    if (init && init->density > 0)
        ind = init->density * 0.1;

        pScreen->width = inw;
        pScreen->height = inh;
        pScreen->mmWidth = (float) inw / ind;
        pScreen->mmHeight = (float) inh / ind;
}

Bool
arcanUnmapFramebuffer(KdScreenInfo * screen)
{
    arcanPriv *priv = screen->card->driver;
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
}

static Bool
ArcanSpecialKey(KeySym sym)
{
    return FALSE;
}

static void
ArcanDisable(void)
{
}

static void
ArcanFini(void)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
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

    *rotations = 0;

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

    /* set the subpixel order */

    KdSetSubpixelOrder(pScreen, scrpriv->randr);
    if (wasEnabled)
        KdEnableScreen(pScreen);

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
    rrScrPrivPtr pScrPriv;

    if (!RRScreenInit(pScreen))
        return FALSE;

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = arcanRandRGetInfo;
    pScrPriv->rrSetConfig = arcanRandRSetConfig;
    return TRUE;
}
#endif

static void
arcanScreenBlockHandler(ScreenPtr pScreen, void* timeout)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

/*
 * This one is rather unfortunate as it seems to be called 'whenever'
 * and we don't know the synch- state of the contents in the buffer
 */

    pScreen->BlockHandler = scrpriv->BlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

    if (scrpriv->damage)
        arcanInternalDamageRedisplay(pScreen);
    else
        arcan_shmif_signal(scrpriv->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE );
}

Bool
arcanCreateColormap(ColormapPtr pmap)
{
    return fbInitializeColormap(pmap);
}

Bool
arcanInitScreen(ScreenPtr pScreen)
{
    pScreen->CreateColormap = arcanCreateColormap;
    return TRUE;
}

Bool
arcanFinishInitScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

#ifdef RANDR
    if (!arcanRandRInit(pScreen))
        return FALSE;
#endif

    scrpriv->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = arcanScreenBlockHandler;

    return TRUE;
}

Bool
arcanCreateResources(ScreenPtr pScreen)
{
    return arcanSetInternalDamage(pScreen);
}

void
arcanPreserve(KdCardInfo * card)
{
}

Bool
arcanEnable(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

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
    return TRUE;
}

void
arcanDisable(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    KdScreenInfo *screen = pScreenPriv->screen;
    arcanScrPriv *scrpriv = screen->driver;

    arcan_shmif_enqueue(scrpriv->acon, &(arcan_event){
        .ext.kind = ARCAN_EVENT(VIEWPORT),
        .ext.viewport = {
            .invisible = false
        }
    });
}

void
arcanRestore(KdCardInfo * card)
{
/* NOOP */
}

void
arcanScreenFini(KdScreenInfo * screen)
{
        struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
        con->user = NULL;
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

    free(priv->base);
    free(priv);
}

void
arcanCloseScreen(ScreenPtr pScreen)
{
    arcanUnsetInternalDamage(pScreen);
}

void
arcanPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs)
{
/*
 * FIXME:
 * should probably forward some cmap_entry thing and invalidate
 * the entire region. Somewhat unsure how this actually works
 */
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
