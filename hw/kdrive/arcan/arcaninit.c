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

#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif
#include "arcan.h"

void
InitCard(char *name)
{
    KdCardInfoAdd(&arcanFuncs, 0);
}

void
InitOutput(ScreenInfo * pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    struct arcan_shmif_cont* con;
    KdPointerInfo* pi;
    KdKeyboardInfo* ki;

    KdAddPointerDriver(&arcanPointerDriver);
    pi = KdParsePointer("arcan");
    KdAddPointer(pi);

    KdAddKeyboardDriver(&arcanKeyboardDriver);
    ki = KdParseKeyboard("arcan");
    KdAddKeyboard(ki);

    KdInitInput();

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
    ErrorF("-aident [str] Set window dynamic identity\n");
    ErrorF("-atitle [str] Set window static identity\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
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
