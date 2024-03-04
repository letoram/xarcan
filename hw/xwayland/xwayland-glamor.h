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

#ifndef XWAYLAND_GLAMOR_H
#define XWAYLAND_GLAMOR_H

#include <xwayland-config.h>

#include <sys/types.h>

#include <wayland-client.h>
#include <xf86drm.h>

#include "xwayland-types.h"
#include "xwayland-glamor-gbm.h"

typedef enum _xwl_glamor_mode_flags{
    XWL_GLAMOR_NONE = 0,
    XWL_GLAMOR_GL = (1 << 0),
    XWL_GLAMOR_GLES = (1 << 1),
    XWL_GLAMOR_DEFAULT = XWL_GLAMOR_GL | XWL_GLAMOR_GLES,
} xwl_glamor_mode_flags;

#ifdef XWL_HAS_GLAMOR

Bool xwl_glamor_init(struct xwl_screen *xwl_screen);

Bool xwl_screen_set_drm_interface(struct xwl_screen *xwl_screen,
                                  uint32_t id, uint32_t version);
struct wl_buffer *xwl_glamor_pixmap_get_wl_buffer(PixmapPtr pixmap);
void xwl_glamor_init_wl_registry(struct xwl_screen *xwl_screen,
                                 struct wl_registry *registry,
                                 uint32_t id, const char *interface,
                                 uint32_t version);
void xwl_glamor_egl_make_current(struct xwl_screen *xwl_screen);
Bool xwl_glamor_check_flip(WindowPtr present_window, PixmapPtr pixmap);
PixmapPtr xwl_glamor_create_pixmap_for_window (struct xwl_window *xwl_window);
Bool xwl_glamor_supports_implicit_sync(struct xwl_screen *xwl_screen);

#ifdef XV
/* glamor Xv Adaptor */
Bool xwl_glamor_xv_init(ScreenPtr pScreen);
#endif /* XV */

#endif /* XWL_HAS_GLAMOR */

#endif /* XWAYLAND_GLAMOR_H */
