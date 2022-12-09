/*
 * Barebones xcb window manager that only works with an inherited descriptor,
 * running from the same process but different thread in the xserver itself,
 * similarly to how the clipboard integration works.
 *
 * Since some applications expect a window manager to be there to some degree,
 * this is mainly to fill that gap even when there is none installed. It does
 * not really do anything except connect a window and run the event loop,
 * letting the outer arcan WM actually move / control / influence through the
 * main connection.
 *
 * There might be a cheaper way doing this creating some X internal resource /
 * fake client that way and forcing the noreset option. Another possible tactic
 * for this WM is to implement the hierarchy to arcan synch here rather than as
 * part of the buffer transfer. It might be less work and less noisy since we
 * only get the relevant hierarchy bits.
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "arcan.h"
#include <inttypes.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xfixes.h>
#include <xcb/xcb_util.h>
#include <poll.h>
#include <errno.h>

_Thread_local static xcb_connection_t* dpy;
_Thread_local static xcb_screen_t* screen;

_Thread_local static xcb_drawable_t wnd_root;
_Thread_local static xcb_drawable_t wnd_wm;

static bool buildWindow(struct proxyWindowData* inWnd)
{
    wnd_wm = xcb_generate_id(dpy);
    screen = xcb_aux_get_screen(dpy, 0);

    xcb_create_window(dpy,
                      XCB_COPY_FROM_PARENT, wnd_wm, wnd_root,
                      0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL
    );
    return true;
}

static bool setupInitState(struct proxyWindowData* inWnd)
{
    dpy = xcb_connect_to_fd(inWnd->socket, NULL);

    if (xcb_connection_has_error(dpy)){
        free(inWnd);
        return false;
    }

    xcb_change_window_attributes(dpy, wnd_root, XCB_CW_EVENT_MASK,
                                 (uint32_t[]){
                                 XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                                 XCB_EVENT_MASK_PROPERTY_CHANGE, 0, 0
    });

    return true;
}

void *arcanMiniWMDispatch(struct proxyWindowData* wnd)
{
    if (!setupInitState(wnd) || !buildWindow(wnd))
        return NULL;

    xcb_generic_event_t* ev;
    while ( (ev = xcb_wait_for_event(dpy), 1) && ev->response_type == 0){
    }
    return NULL;
}
