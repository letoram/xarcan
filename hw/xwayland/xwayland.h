/*
 * Copyright © 2014 Intel Corporation
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

#ifndef XWAYLAND_H
#define XWAYLAND_H

#include <dix-config.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <wayland-client.h>

#include <X11/X.h>

#include <fb.h>
#include <input.h>
#include <dix.h>
#include <randrstr.h>
#include <exevents.h>

#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"

struct xwl_screen {
    int width;
    int height;
    int depth;
    ScreenPtr screen;
    int expecting_event;
    enum RootClipMode root_clip_mode;

    int wm_fd;
    int listen_fds[5];
    int listen_fd_count;
    int rootless;
    int glamor;

    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr CreateWindow;
    DestroyWindowProcPtr DestroyWindow;
    RealizeWindowProcPtr RealizeWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    XYToWindowProcPtr XYToWindow;

    struct xorg_list output_list;
    struct xorg_list seat_list;
    struct xorg_list damage_window_list;

    int wayland_fd;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_registry *input_registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_shell *shell;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct zwp_pointer_constraints_v1 *pointer_constraints;

    uint32_t serial;

#define XWL_FORMAT_ARGB8888 (1 << 0)
#define XWL_FORMAT_XRGB8888 (1 << 1)
#define XWL_FORMAT_RGB565   (1 << 2)

    int prepare_read;
    int wait_flush;

    char *device_name;
    int drm_fd;
    int fd_render_node;
    struct wl_drm *drm;
    uint32_t formats;
    uint32_t capabilities;
    void *egl_display, *egl_context;
    struct gbm_device *gbm;
    struct glamor_context *glamor_ctx;

    Atom allow_commits_prop;
};

struct xwl_window {
    struct xwl_screen *xwl_screen;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    WindowPtr window;
    DamagePtr damage;
    struct xorg_list link_damage;
    struct wl_callback *frame_callback;
    Bool allow_commits;
};

#define MODIFIER_META 0x01

struct xwl_touch {
    struct xwl_window *window;
    int32_t id;
    int x, y;
    struct xorg_list link_touch;
};

struct xwl_pointer_warp_emulator {
    struct xwl_seat *xwl_seat;
    struct xwl_window *locked_window;
    struct zwp_locked_pointer_v1 *locked_pointer;
};

struct xwl_seat {
    DeviceIntPtr pointer;
    DeviceIntPtr relative_pointer;
    DeviceIntPtr keyboard;
    DeviceIntPtr touch;
    struct xwl_screen *xwl_screen;
    struct wl_seat *seat;
    struct wl_pointer *wl_pointer;
    struct zwp_relative_pointer_v1 *wp_relative_pointer;
    struct wl_keyboard *wl_keyboard;
    struct wl_touch *wl_touch;
    struct wl_array keys;
    struct xwl_window *focus_window;
    uint32_t id;
    uint32_t pointer_enter_serial;
    struct xorg_list link;
    CursorPtr x_cursor;
    struct wl_surface *cursor;
    struct wl_callback *cursor_frame_cb;
    Bool cursor_needs_update;
    WindowPtr last_xwindow;

    struct xorg_list touches;

    size_t keymap_size;
    char *keymap;
    struct wl_surface *keyboard_focus;

    struct xorg_list sync_pending;

    struct xwl_pointer_warp_emulator *pointer_warp_emulator;

    struct xwl_window *cursor_confinement_window;
    struct zwp_confined_pointer_v1 *confined_pointer;

    struct {
        Bool has_absolute;
        wl_fixed_t x;
        wl_fixed_t y;

        Bool has_relative;
        double dx;
        double dy;
        double dx_unaccel;
        double dy_unaccel;
    } pending_pointer_event;
};

struct xwl_output {
    struct xorg_list link;
    struct wl_output *output;
    uint32_t server_output_id;
    struct xwl_screen *xwl_screen;
    RROutputPtr randr_output;
    RRCrtcPtr randr_crtc;
    int32_t x, y, width, height, refresh;
    Rotation rotation;
};

struct xwl_pixmap;

void xwl_sync_events (struct xwl_screen *xwl_screen);

Bool xwl_screen_init_cursor(struct xwl_screen *xwl_screen);

struct xwl_screen *xwl_screen_get(ScreenPtr screen);

void xwl_seat_set_cursor(struct xwl_seat *xwl_seat);

void xwl_seat_destroy(struct xwl_seat *xwl_seat);

void xwl_seat_clear_touch(struct xwl_seat *xwl_seat, WindowPtr window);

void xwl_seat_emulate_pointer_warp(struct xwl_seat *xwl_seat,
                                   struct xwl_window *xwl_window,
                                   SpritePtr sprite,
                                   int x, int y);

void xwl_seat_destroy_pointer_warp_emulator(struct xwl_seat *xwl_seat);

void xwl_seat_cursor_visibility_changed(struct xwl_seat *xwl_seat);

void xwl_seat_confine_pointer(struct xwl_seat *xwl_seat,
                              struct xwl_window *xwl_window);
void xwl_seat_unconfine_pointer(struct xwl_seat *xwl_seat);

Bool xwl_screen_init_output(struct xwl_screen *xwl_screen);

struct xwl_output *xwl_output_create(struct xwl_screen *xwl_screen,
                                     uint32_t id);

void xwl_output_destroy(struct xwl_output *xwl_output);

void xwl_output_remove(struct xwl_output *xwl_output);

RRModePtr xwayland_cvt(int HDisplay, int VDisplay,
                       float VRefresh, Bool Reduced, Bool Interlaced);

void xwl_pixmap_set_private(PixmapPtr pixmap, struct xwl_pixmap *xwl_pixmap);
struct xwl_pixmap *xwl_pixmap_get(PixmapPtr pixmap);


Bool xwl_shm_create_screen_resources(ScreenPtr screen);
PixmapPtr xwl_shm_create_pixmap(ScreenPtr screen, int width, int height,
                                int depth, unsigned int hint);
Bool xwl_shm_destroy_pixmap(PixmapPtr pixmap);
struct wl_buffer *xwl_shm_pixmap_get_wl_buffer(PixmapPtr pixmap);


Bool xwl_glamor_init(struct xwl_screen *xwl_screen);

Bool xwl_screen_init_glamor(struct xwl_screen *xwl_screen,
                         uint32_t id, uint32_t version);
struct wl_buffer *xwl_glamor_pixmap_get_wl_buffer(PixmapPtr pixmap);

#ifdef XV
/* glamor Xv Adaptor */
Bool xwl_glamor_xv_init(ScreenPtr pScreen);
#endif

#ifdef XF86VIDMODE
void xwlVidModeExtensionInit(void);
#endif

#endif
