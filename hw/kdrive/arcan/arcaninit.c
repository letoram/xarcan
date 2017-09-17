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
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "arcan.h"
#include "glx_extinit.h"

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

void
InitCard(char *name)
{
    trace("ArcanInit:InitCard");
    KdCardInfoAdd(&arcanFuncs, 0);
}

static const ExtensionModule arcanExtensions[] = {
#ifdef GLXEXT
	{ GlxExtensionInit, "GLX", &noGlxExtension }
#endif
};

void
InitOutput(ScreenInfo * pScreenInfo, int argc, char **argv)
{
/*
    int depths[] = {1, 4, 8, 15, 16, 24, 32};
    int bpp[] = {1, 8, 8, 16, 16, 32, 32};
    trace("ArcanInit:InitOutput");
    for (int i = 0; i < 7; i++){
        pScreenInfo->formats[i].depth = depths[i];
        pScreenInfo->formats[i].bitsPerPixel = bpp[i];
        pScreenInfo->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }
    pScreenInfo->imageByteOrder = IMAGE_BYTE_ORDER;
    pScreenInfo->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    pScreenInfo->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    pScreenInfo->bitmapBitOrder = BITMAP_BIT_ORDER;
    pScreenInfo->numPixmapFormats = ARRAY_SIZE(depths);
 */
    static bool gotext;
    if (!gotext){
        LoadExtensionList(arcanExtensions, ARRAY_SIZE(arcanExtensions), TRUE);
        gotext = true;
    }
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    struct arcan_shmif_cont* con;
    KdPointerInfo* pi;
    KdKeyboardInfo* ki;

    trace("ArcanInit:InitInput");
    KdAddPointerDriver(&arcanPointerDriver);
    pi = KdParsePointer("arcan");
    KdAddPointer(pi);

    KdAddKeyboardDriver(&arcanKeyboardDriver);
    ki = KdParseKeyboard("arcan");
    KdAddKeyboard(ki);

    KdInitInput();

/*
 * 	DEBUGGING - enable this help somewhat
    LogInit("yeahlog", ".old");
		LogSetParameter(XLOG_FILE_VERBOSITY, 100000);
 */

/* Register won't work unless we're in init input state */
    con = arcan_shmif_primary(SHMIF_INPUT);
    if (con){
        KdRegisterFd(con->epipe, (void*) arcanFlushEvents, con);
    }
    else {
        ErrorF("kdrive:arcan - No Primary Segment during InitInput\n");
    }
}

void
CloseInput(void)
{
    struct arcan_shmif_cont* con = arcan_shmif_primary(SHMIF_INPUT);
    trace("ArcanInit:CloseInput");
    if (con){
        KdUnregisterFd(con, con->epipe, FALSE);
    }
    KdCloseInput();
}

#ifdef DDXBEFORERESET
void
ddxBeforeReset(void)
{
}
#endif

void
ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("\nXarcan Option Usage:\n");
    ErrorF("-nodynamic             Disable connection-controlled resize\n");
    ErrorF("-aident [str]          Set window dynamic identity\n");
    ErrorF("-atitle [str]          Set window static identity\n");
#ifdef GLAMOR
    ErrorF("-glamor                Enable glamor rendering\n");
#endif
    ErrorF("-nodri3                Disable DRI3- support\n");
    ErrorF("-accel_cursor          Enable accelerated cursor\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
#ifdef GLAMOR
    if (strcmp(argv[i], "-glamor") == 0){
		arcanGlamor = 1;
        arcanFuncs.initAccel = arcanGlamorInit;
        arcanFuncs.enableAccel = arcanGlamorEnable;
        arcanFuncs.disableAccel = arcanGlamorDisable;
        arcanFuncs.finiAccel = arcanGlamorFini;
        return 1;
    }
    else
#endif
    if (strcmp(argv[i], "-nodri3") == 0){
        arcanConfigPriv.no_dri3 = true;
        return 1;
    }
    else if (strcmp(argv[i], "-aident") == 0){
        if ((i+1) < argc){
            return 2;
        }
        arcanConfigPriv.ident = strdup(argv[i+1]);
        ddxUseMsg();
        exit(1);
    }
    else if (strcmp(argv[i], "-atitle") == 0){
        if ((i+1) < argc){
            return 2;
        }
        arcanConfigPriv.title = strdup(argv[i+1]);
        ddxUseMsg();
        exit(1);
    }
    else if (strcmp(argv[i], "-nodynamic") == 0){
        arcanConfigPriv.no_dynamic_resize = true;
        return 1;
    }
    else if (strcmp(argv[i], "-accel_cursor") == 0){
        arcanConfigPriv.accel_cursor = true;
    }
/*
 * rather useless, txpass would act pretty much the same as glamor,
 * except for the actual drawing that is
 *
    else if (strcmp(argv[i], "-txpass") == 0){
        arcanConfigPriv.txpass = true;
        return 1;
    }
 */
    return KdProcessArgument(argc, argv, i);
}

void
OsVendorInit(void)
{
    KdOsInit(&arcanOsFuncs);
}

KdCardFuncs arcanFuncs = {
    arcanCardInit,               /* cardinit */
    arcanScreenInit,             /* scrinit */
    arcanInitScreen,             /* initScreen */
    arcanFinishInitScreen,       /* finishInitScreen */
    arcanCreateResources,        /* createRes */
    arcanPreserve,               /* preserve */
    arcanEnable,                 /* enable */
    arcanDPMS,                   /* dpms */
    arcanDisable,                /* disable */
    arcanRestore,                /* restore */
    arcanScreenFini,             /* scrfini */
    arcanCardFini,               /* cardfini */

    0,                          /* initCursor */
    0,                          /* enableCursor */
    0,                          /* disableCursor */
    0,                          /* finiCursor */
    0,                          /* recolorCursor */

    0,                          /* initAccel */
    0,                          /* enableAccel */
    0,                          /* disableAccel */
    0,                          /* finiAccel */

    arcanGetColors,              /* getColors */
    arcanPutColors,              /* putColors */
    arcanCloseScreen             /* close Screen */
};
