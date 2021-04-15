/*
 * Copyright © 2011-2014 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <xwayland-config.h>

#include <stdio.h>

#include <X11/Xatom.h>
#include <selection.h>
#include <micmap.h>
#include <misyncshm.h>
#include <compositeext.h>
#include <compint.h>
#include <glx_extinit.h>
#include <os.h>
#include <xserver_poll.h>
#include <propertyst.h>
#include <version-config.h>

#include "xwayland-screen.h"
#include "xwayland-vidmode.h"

#ifdef XF86VIDMODE
#include <X11/extensions/xf86vmproto.h>
extern _X_EXPORT Bool noXFree86VidModeExtension;
#endif

void
ddxGiveUp(enum ExitCode error)
{
}

void
OsVendorInit(void)
{
    if (serverGeneration == 1)
        ForceClockId(CLOCK_MONOTONIC);
}

void
OsVendorFatalError(const char *f, va_list args)
{
}

#if defined(DDXBEFORERESET)
void
ddxBeforeReset(void)
{
    return;
}
#endif

#if INPUTTHREAD
/** This function is called in Xserver/os/inputthread.c when starting
    the input thread. */
void
ddxInputThreadInit(void)
{
}
#endif

void
ddxUseMsg(void)
{
    ErrorF("-rootless              run rootless, requires wm support\n");
    ErrorF("-wm fd                 create X client for wm on given fd\n");
    ErrorF("-initfd fd             add given fd as a listen socket for initialization clients\n");
    ErrorF("-listenfd fd           add given fd as a listen socket\n");
    ErrorF("-listen fd             deprecated, use \"-listenfd\" instead\n");
#ifdef XWL_HAS_EGLSTREAM
    ErrorF("-eglstream             use eglstream backend for nvidia GPUs\n");
#endif
    ErrorF("-shm                   use shared memory for passing buffers\n");
    ErrorF("-verbose [n]           verbose startup messages\n");
    ErrorF("-version               show the server version and exit\n");
}

static int init_fd = -1;
static int wm_fd = -1;
static int listen_fds[5] = { -1, -1, -1, -1, -1 };
static int listen_fd_count = 0;
static int verbosity = 0;

static void
xwl_show_version(void)
{
    ErrorF("%s Xwayland %s (%d)\n", VENDOR_NAME, VENDOR_MAN_VERSION, VENDOR_RELEASE);
    ErrorF("X Protocol Version %d, Revision %d\n", X_PROTOCOL, X_PROTOCOL_REVISION);
#if defined(BUILDERSTRING)
    if (strlen(BUILDERSTRING))
        ErrorF("%s\n", BUILDERSTRING);
#endif
}

static void
xwl_add_listen_fd(int argc, char *argv[], int i)
{
    NoListenAll = TRUE;
    if (listen_fd_count == ARRAY_SIZE(listen_fds))
        FatalError("Too many -listen arguments given, max is %zu\n",
                   ARRAY_SIZE(listen_fds));

    listen_fds[listen_fd_count++] = atoi(argv[i + 1]);
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    if (strcmp(argv[i], "-rootless") == 0) {
        return 1;
    }
    else if (strcmp(argv[i], "-listen") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);

        /* Not an FD */
        if (!isdigit(*argv[i + 1]))
            return 0;

        LogMessageVerb(X_WARNING, 0, "Option \"-listen\" for file descriptors is deprecated\n"
                                     "Please use \"-listenfd\" instead.\n");

        xwl_add_listen_fd (argc, argv, i);
        return 2;
    }
    else if (strcmp(argv[i], "-listenfd") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);

        xwl_add_listen_fd (argc, argv, i);
        return 2;
    }
    else if (strcmp(argv[i], "-wm") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        wm_fd = atoi(argv[i + 1]);
        return 2;
    }
    else if (strcmp(argv[i], "-initfd") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        init_fd = atoi(argv[i + 1]);
        return 2;
    }
    else if (strcmp(argv[i], "-shm") == 0) {
        return 1;
    }
    else if (strcmp(argv[i], "-verbose") == 0) {
        if (++i < argc && argv[i]) {
            char *end;
            long val;

            val = strtol(argv[i], &end, 0);
            if (*end == '\0') {
                verbosity = val;
                LogSetParameter(XLOG_VERBOSITY, verbosity);
                return 2;
            }
        }
        LogSetParameter(XLOG_VERBOSITY, ++verbosity);
        return 1;
    }
    else if (strcmp(argv[i], "-eglstream") == 0) {
        return 1;
    }
    else if (strcmp(argv[i], "-version") == 0) {
        xwl_show_version();
        exit(0);
    }

    return 0;
}

static CARD32
add_client_fd(OsTimerPtr timer, CARD32 time, void *arg)
{
    if (!AddClientOnOpenFD(wm_fd))
        FatalError("Failed to add wm client\n");

    TimerFree(timer);

    return 0;
}

static void
listen_on_fds(void)
{
    int i;

    for (i = 0; i < listen_fd_count; i++)
        ListenOnOpenFD(listen_fds[i], FALSE);
}

static void
wm_selection_callback(CallbackListPtr *p, void *data, void *arg)
{
    SelectionInfoRec *info = arg;
    struct xwl_screen *xwl_screen = data;
    static const char atom_name[] = "WM_S0";
    static Atom atom_wm_s0;

    if (atom_wm_s0 == None)
        atom_wm_s0 = MakeAtom(atom_name, strlen(atom_name), TRUE);
    if (info->selection->selection != atom_wm_s0 ||
        info->kind != SelectionSetOwner)
        return;

    listen_on_fds();

    DeleteCallback(&SelectionCallback, wm_selection_callback, xwl_screen);
}

_X_NORETURN
static void _X_ATTRIBUTE_PRINTF(1, 0)
xwl_log_handler(const char *format, va_list args)
{
    char msg[256];

    vsnprintf(msg, sizeof msg, format, args);
    FatalError("%s", msg);
}

static const ExtensionModule xwayland_extensions[] = {
#ifdef XF86VIDMODE
    { xwlVidModeExtensionInit, XF86VIDMODENAME, &noXFree86VidModeExtension },
#endif
};

void
InitOutput(ScreenInfo * screen_info, int argc, char **argv)
{
    int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int bpp[] =    { 1, 8, 8, 16, 16, 32, 32 };
    int i;

    for (i = 0; i < ARRAY_SIZE(depths); i++) {
        screen_info->formats[i].depth = depths[i];
        screen_info->formats[i].bitsPerPixel = bpp[i];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = ARRAY_SIZE(depths);

    if (serverGeneration == 1)
        LoadExtensionList(xwayland_extensions,
                          ARRAY_SIZE(xwayland_extensions), FALSE);

    wl_log_set_handler_client(xwl_log_handler);

    if (AddScreen(xwl_screen_init, argc, argv) == -1) {
        FatalError("Couldn't add screen\n");
    }

    xorgGlxCreateVendor();

    LocalAccessScopeUser();

    if (wm_fd >= 0 || init_fd >= 0) {
        if (wm_fd >= 0)
            TimerSet(NULL, 0, 1, add_client_fd, NULL);
        if (init_fd >= 0)
            ListenOnOpenFD(init_fd, FALSE);
        AddCallback(&SelectionCallback, wm_selection_callback, NULL);
    }
    else if (listen_fd_count > 0) {
        listen_on_fds();
    }
}
