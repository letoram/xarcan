/*
 * Copyright © 2018 Roman Gilg
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

#ifndef XWAYLAND_PRESENT_H
#define XWAYLAND_PRESENT_H

#include <xwayland-config.h>

#include <dix.h>
#include <present_priv.h>

#ifdef GLAMOR_HAS_GBM
struct xa_present_window {
    WindowPtr window;

    struct xorg_list frame_callback_list;

    uint64_t msc;
    uint64_t ust;

    OsTimerPtr frame_timer;
    /* Timestamp when the current timer was first armed */
    CARD32 timer_armed;

    struct wl_callback *sync_callback;

    struct xorg_list wait_list;
    struct xorg_list flip_queue;
    struct xorg_list idle_queue;

    present_vblank_ptr flip_active;
};

struct xa_present_event {
    present_vblank_rec vblank;
    PixmapPtr pixmap;
		Bool async_may_tear;
};

void xa_present_reset_timer(struct xa_present_window *);
void xa_present_msc_bump(struct xa_present_window *xa_present_window, uint64_t msc);
Bool xa_present_init(ScreenPtr screen);
void xa_present_cleanup(WindowPtr window);
void xa_present_unrealize_window(struct xa_present_window*);
void xa_present_buffer_release(struct xa_present_window*);
struct xa_present_window* xa_present_window_priv(WindowPtr window);
#endif /* GLAMOR_HAS_GBM */

#endif /* XWAYLAND_PRESENT_H */
