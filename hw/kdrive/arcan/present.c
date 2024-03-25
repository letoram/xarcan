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

/*
 * modified xwayland-present to match types, and instead of hooking up to
 * frame-callbacks we are triggered by the STEPFRAME event in each pixmap
 * event-loop.
 *
 * notes: timer_len_copy should be derived from _initial()
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#define WANT_ARCAN_SHMIF_HELPER
#include "arcan.h"
#include <windowstr.h>
#include <present.h>

#include "xa-present.h"
#include "glamor.h"

#define XA_PRESENT_CAPS PresentCapabilityAsync | PresentCapabilityAsyncMayTear

static DevPrivateKeyRec xa_present_window_private_key;

struct xa_present_window *
xa_present_window_priv(WindowPtr window)
{
    return dixGetPrivate(&window->devPrivates,
                         &xa_present_window_private_key);
}

static struct xa_present_window *
xa_present_window_get_priv(WindowPtr window)
{
    struct xa_present_window *xa_present_window = xa_present_window_priv(window);

    if (xa_present_window == NULL) {
        xa_present_window = calloc (1, sizeof (struct xa_present_window));
        if (!xa_present_window)
            return NULL;

        xa_present_window->window = window;
        xa_present_window->msc = 1;
        xa_present_window->ust = GetTimeInMicros();

        xorg_list_init(&xa_present_window->wait_list);
        xorg_list_init(&xa_present_window->flip_queue);
        xorg_list_init(&xa_present_window->idle_queue);

        dixSetPrivate(&window->devPrivates,
                      &xa_present_window_private_key,
                      xa_present_window);
    }

    return xa_present_window;
}

static struct xa_present_event *
xa_present_event_from_id(WindowPtr present_window, uint64_t event_id)
{
    present_window_priv_ptr window_priv = present_get_window_priv(present_window, TRUE);
    struct xa_present_event *event;

    xorg_list_for_each_entry(event, &window_priv->vblank, vblank.window_list) {
        if (event->vblank.event_id == event_id)
            return event;
    }
    return NULL;
}

static struct xa_present_event *
xa_present_event_from_vblank(present_vblank_ptr vblank)
{
    return container_of(vblank, struct xa_present_event, vblank);
}

static present_vblank_ptr
xa_present_get_pending_flip(struct xa_present_window *xa_present_window)
{
    present_vblank_ptr flip_pending;

    if (xorg_list_is_empty(&xa_present_window->flip_queue))
        return NULL;

    flip_pending = xorg_list_first_entry(&xa_present_window->flip_queue, present_vblank_rec,
                                         event_queue);

    if (flip_pending->queued)
        return NULL;

    return flip_pending;
}

static inline Bool
xa_present_has_pending_events(struct xa_present_window *xa_present_window)
{
    present_vblank_ptr flip_pending = xa_present_get_pending_flip(xa_present_window);

    return (flip_pending && flip_pending->sync_flip) ||
           !xorg_list_is_empty(&xa_present_window->wait_list);
}

static void
xa_present_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc);

static uint32_t
xa_present_query_capabilities(present_screen_priv_ptr screen_priv)
{
    return XA_PRESENT_CAPS;
}

static int
xa_present_get_ust_msc(ScreenPtr screen,
                        WindowPtr present_window,
                        uint64_t *ust,
                        uint64_t *msc)
{
    struct xa_present_window *xa_present_window = xa_present_window_get_priv(present_window);
    if (!xa_present_window)
        return BadAlloc;

    *ust = xa_present_window->ust;
    *msc = xa_present_window->msc;

    return Success;
}

/*
 * When the wait fence or previous flip is completed, it's time
 * to re-try the request
 */
static void
xa_present_re_execute(present_vblank_ptr vblank)
{
    uint64_t ust = 0, crtc_msc = 0;

    (void) xa_present_get_ust_msc(vblank->screen, vblank->window, &ust, &crtc_msc);
    xa_present_execute(vblank, ust, crtc_msc);
}

static void
xa_present_flip_try_ready(struct xa_present_window *xa_present_window)
{
    present_vblank_ptr vblank;

    xorg_list_for_each_entry(vblank, &xa_present_window->flip_queue, event_queue) {
        if (vblank->queued) {
            xa_present_re_execute(vblank);
            return;
        }
    }
}

static void
xa_present_release_pixmap(struct xa_present_event *event)
{
    if (!event->pixmap)
        return;

/*
 * shouldn't be needed, buffer is tied to the shmif context in the pixmap
 * and we disassociate, not delete
 	xwl_pixmap_del_buffer_release_cb(event->pixmap);
 */
    dixDestroyPixmap(event->pixmap, event->pixmap->drawable.id);
    event->pixmap = NULL;
}

static void
xa_present_free_event(struct xa_present_event *event)
{
    xa_present_release_pixmap(event);
    xorg_list_del(&event->vblank.event_queue);
    present_vblank_destroy(&event->vblank);
}

static void
xa_present_free_idle_vblank(present_vblank_ptr vblank)
{
    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
    xa_present_free_event(xa_present_event_from_vblank(vblank));
}

static WindowPtr
xa_present_toplvl_pixmap_window(WindowPtr window)
{
    ScreenPtr       screen = window->drawable.pScreen;
    PixmapPtr       pixmap = (*screen->GetWindowPixmap)(window);
    WindowPtr       w = window;
    WindowPtr       next_w;

    while(w->parent) {
        next_w = w->parent;
        if ( (*screen->GetWindowPixmap)(next_w) != pixmap) {
            break;
        }
        w = next_w;
    }
    return w;
}

static void
xa_present_flips_stop(WindowPtr window)
{
    struct xa_present_window *xa_present_window = xa_present_window_priv(window);
    present_vblank_ptr vblank, tmp;

    /* Free any left over idle vblanks */
    xorg_list_for_each_entry_safe(vblank, tmp, &xa_present_window->idle_queue, event_queue)
        xa_present_free_idle_vblank(vblank);

    if (xa_present_window->flip_active) {
        struct xa_present_event *event;

        vblank = xa_present_window->flip_active;
        event = xa_present_event_from_vblank(vblank);
        if (event->pixmap)
            xa_present_free_idle_vblank(vblank);
        else
            xa_present_free_event(event);

        xa_present_window->flip_active = NULL;
    }

    xa_present_flip_try_ready(xa_present_window);
}

static void
xa_present_flip_notify_vblank(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    struct xa_present_window *xa_present_window = xa_present_window_priv(window);
    uint8_t mode = PresentCompleteModeFlip;

    DebugPresent(("\ttype=notify_flip:id=%" PRIu64 ":ptr=%p:msc=%" PRIu64 ":tgt_msc=%" PRIu64 ":pmap=%08" PRIx32 ":wnd=%08" PRIx32 "\n",
                  vblank->event_id, vblank, vblank->exec_msc, vblank->target_msc,
                  vblank->pixmap ? vblank->pixmap->drawable.id : 0,
                  vblank->window ? vblank->window->drawable.id : 0));

    assert (&vblank->event_queue == xa_present_window->flip_queue.next);

    xorg_list_del(&vblank->event_queue);

    if (xa_present_window->flip_active) {
        struct xa_present_event *event =
            xa_present_event_from_vblank(xa_present_window->flip_active);

        if (!event->pixmap)
            xa_present_free_event(event);
        else
            /* Put the previous flip in the idle_queue and wait for further notice from
             * the Wayland compositor
             */
            xorg_list_append(&xa_present_window->flip_active->event_queue, &xa_present_window->idle_queue);
    }

    xa_present_window->flip_active = vblank;

    if (vblank->reason == PRESENT_FLIP_REASON_BUFFER_FORMAT)
        mode = PresentCompleteModeSuboptimalCopy;

    present_vblank_notify(vblank, PresentCompleteKindPixmap, mode, ust, crtc_msc);

    if (vblank->abort_flip)
        xa_present_flips_stop(window);

    xa_present_flip_try_ready(xa_present_window);
}

static void
xa_present_update_window_crtc(present_window_priv_ptr window_priv, RRCrtcPtr crtc, uint64_t new_msc)
{
    /* Crtc unchanged, no offset. */
    if (crtc == window_priv->crtc)
        return;

    /* No crtc earlier to offset against, just set the crtc. */
    if (window_priv->crtc == PresentCrtcNeverSet) {
        window_priv->msc_offset = 0;
        window_priv->crtc = crtc;
        return;
    }

    /* In window-mode the last correct msc-offset is always kept
     * in window-priv struct because msc is saved per window and
     * not per crtc as in screen-mode.
     */
    window_priv->msc_offset += new_msc - window_priv->msc;
    window_priv->crtc = crtc;
}


void
xa_present_cleanup(WindowPtr window)
{
    struct xa_present_window *xa_present_window = xa_present_window_priv(window);
    present_window_priv_ptr window_priv = present_window_priv(window);
    struct xa_present_event *event, *tmp;

    if (!xa_present_window)
        return;

    /* Clear remaining events */
    xorg_list_for_each_entry_safe(event, tmp, &window_priv->vblank, vblank.window_list)
        xa_present_free_event(event);

   /* Remove from privates so we don't try to access it later */
    dixSetPrivate(&window->devPrivates,
                  &xa_present_window_private_key,
                  NULL);

    free(xa_present_window);
}

void
xa_present_buffer_release(struct xa_present_window* window)
{
    present_vblank_ptr vblank = xa_present_get_pending_flip(window);
    if (!vblank)
        return;

    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);

/*
    xa_present_window = xa_present_window_priv(vblank->window);
    if (xa_present_window->flip_active == vblank ||
        xa_present_get_pending_flip(xa_present_window) == vblank)
        xa_present_release_pixmap(event);
    else
        xa_present_free_event(event);
 */
}

void
xa_present_msc_bump(struct xa_present_window *xa_present_window, uint64_t msc)
{
    present_vblank_ptr flip_pending = xa_present_get_pending_flip(xa_present_window);
    xa_present_window->msc = msc;
    present_vblank_ptr vblank, tmp;

    xa_present_window->ust = GetTimeInMicros();

    if (flip_pending && flip_pending->sync_flip)
        xa_present_flip_notify_vblank(flip_pending, xa_present_window->ust, msc);

    xorg_list_for_each_entry_safe(vblank, tmp, &xa_present_window->wait_list, event_queue) {
        if (vblank->exec_msc <= msc) {
            DebugPresent(("\ttype=msc-reply:id=%" PRIu64 ":ust=%" PRIu64 ":msc=%" PRIu64 "\n",
                          vblank->event_id, xa_present_window->ust, msc));

            xa_present_execute(vblank, xa_present_window->ust, msc);
        }
        else
            DebugPresent(("type=wait_for:tgt_msc=%"PRIu64":msc=%"PRIu64"\n", vblank->exec_msc, msc));
    }
}

static RRCrtcPtr
xa_present_get_crtc(present_screen_priv_ptr screen_priv,
                     WindowPtr present_window)
{
    struct xa_present_window *xa_present_window = xa_present_window_get_priv(present_window);
    rrScrPrivPtr rr_private;

    if (xa_present_window == NULL)
        return NULL;

    rr_private = rrGetScrPriv(present_window->drawable.pScreen);

    if (rr_private->numCrtcs == 0)
        return NULL;

    return rr_private->crtcs[0];
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has passed
 */
static int
xa_present_queue_vblank(ScreenPtr screen,
                         WindowPtr present_window,
                         RRCrtcPtr crtc,
                         uint64_t event_id,
                         uint64_t msc)
{
    struct xa_present_window *xa_present_window = xa_present_window_get_priv(present_window);
    struct xa_present_event *event = xa_present_event_from_id(present_window, event_id);

    if (!event) {
        ErrorF("present: Error getting event\n");
        return BadImplementation;
    }

    event->vblank.exec_msc = msc;

    xorg_list_del(&event->vblank.event_queue);
    xorg_list_append(&event->vblank.event_queue, &xa_present_window->wait_list);

    return Success;
}

/*
 * Remove a pending vblank event so that it is not reported
 * to the extension
 */
static void
xa_present_abort_vblank(ScreenPtr screen,
                         WindowPtr present_window,
                         RRCrtcPtr crtc,
                         uint64_t event_id,
                         uint64_t msc)
{
    static Bool called;

    if (called)
        return;

    /* xa_present_cleanup should have cleaned up everything,
     * present_free_window_vblank shouldn't need to call this.
     */
    ErrorF("Unexpected call to %s:\n", __func__);
    xorg_backtrace();
}

static void
xa_present_flush(WindowPtr window)
{
    glamor_block_handler(window->drawable.pScreen);
}

static Bool
xa_present_check_flip(RRCrtcPtr crtc,
                       WindowPtr present_window,
                       PixmapPtr pixmap,
                       Bool sync_flip,
                       RegionPtr valid,
                       int16_t x_off,
                       int16_t y_off,
                       PresentFlipReason *reason)
{
    WindowPtr toplvl_window = xa_present_toplvl_pixmap_window(present_window);
/*    struct xwl_window *xwl_window = xwl_window_from_window(present_window); */
/*    ScreenPtr screen = pixmap->drawable.pScreen; */

    if (reason)
        *reason = PRESENT_FLIP_REASON_UNKNOWN;

/*
 * if (!xwl_window)
        return FALSE;
 */

    if (!crtc)
        return FALSE;

    /* Source pixmap must align with window exactly */
    if (x_off || y_off)
        return FALSE;

    /* Valid area must contain window (for simplicity for now just never flip when one is set). */
    if (valid)
        return FALSE;

    /* Flip pixmap must have same dimensions as window */
    if (present_window->drawable.width != pixmap->drawable.width ||
            present_window->drawable.height != pixmap->drawable.height)
        return FALSE;

    /* Window must be same region as toplevel window */
    if ( !RegionEqual(&present_window->winSize, &toplvl_window->winSize) )
        return FALSE;

    /* Can't flip if window clipped by children */
    if (!RegionEqual(&present_window->clipList, &present_window->winSize))
        return FALSE;

/*
    if (!xwl_glamor_check_flip(pixmap))
        return FALSE;
  */

    /* Can't flip if the window pixmap doesn't match the xwl_window parent
     * window's, e.g. because a client redirected this window or one of its
     * parents.
     */
/*
 * if (screen->GetWindowPixmap(xwl_window->window) != screen->GetWindowPixmap(present_window))
        return FALSE;
*/

    /*
     * We currently only allow flips of windows, that have the same
     * dimensions as their xwl_window parent window. For the case of
     * different sizes subsurfaces are presumably the way forward.
     */

    /*if (!RegionEqual(&xwl_window->window->winSize, &present_window->winSize))
        return FALSE;
    */

    return TRUE;
}

/*
 * 'window' is being reconfigured. Check to see if it is involved
 * in flipping and clean up as necessary.
 */
static void
xa_present_check_flip_window (WindowPtr window)
{
    struct xa_present_window *xa_present_window = xa_present_window_priv(window);
    present_window_priv_ptr window_priv = present_window_priv(window);
    present_vblank_ptr      flip_pending;
    present_vblank_ptr      flip_active;
    present_vblank_ptr      vblank;
    PresentFlipReason       reason;

    /* If this window hasn't ever been used with Present, it can't be
     * flipping
     */
    if (!xa_present_window || !window_priv)
        return;

    flip_pending = xa_present_get_pending_flip(xa_present_window);
    flip_active = xa_present_window->flip_active;

    if (flip_pending) {
        if (!xa_present_check_flip(flip_pending->crtc, flip_pending->window, flip_pending->pixmap,
                                    flip_pending->sync_flip, flip_pending->valid, 0, 0, NULL))
            flip_pending->abort_flip = TRUE;
    } else if (flip_active) {
        if (!xa_present_check_flip(flip_active->crtc, flip_active->window, flip_active->pixmap,
                                    flip_active->sync_flip, flip_active->valid, 0, 0, NULL))
            xa_present_flips_stop(window);
    }

    /* Now check any queued vblanks */
    xorg_list_for_each_entry(vblank, &window_priv->vblank, window_list) {
        if (vblank->queued && vblank->flip &&
                !xa_present_check_flip(vblank->crtc, window, vblank->pixmap,
                                        vblank->sync_flip, vblank->valid, 0, 0, &reason)) {
            vblank->flip = FALSE;
            vblank->reason = reason;
        }
    }
}

/*
 * Clean up any pending or current flips for this window
 */
static void
xa_present_clear_window_flip(WindowPtr window)
{
    /* xa_present_cleanup cleaned up everything */
}

static Bool
xa_present_flip(present_vblank_ptr vblank, RegionPtr damage)
{
    arcanWindowPriv* awnd = arcanWindowFromWnd(vblank->window);
    arcanPixmapPriv* apmap = arcanPixmapFromPixmap(vblank->pixmap);

    if (!awnd || !apmap){
        return FALSE;
    }

		struct arcan_shmif_cont* C = NULL;
		if (!awnd->shmif){
		    if (!arcanConfigPriv.redirect){
            C = arcan_shmif_primary(SHMIF_INPUT);
		    }
		}
		else {
		    C = awnd->shmif;
		}

/* means we can skip the signalling from the other part of the chain */
    awnd->usePresent = true;

    if (apmap->texture){
        arcan_shmifext_setup(C,
                             (struct arcan_shmifext_setup){
                                                           .no_context = 1,
                                                           .shared_context = 0
                             });

    C->dirty.x1 = 0;
    C->dirty.y1 = 0;
    C->dirty.x2 = C->w;
    C->dirty.y2 = C->h;

#if ASHMIF_VERSION_MINOR > 16
        /* arcan_shmifext_export_image(
				 *     con, display, apmap->texture, 4, &planes);
				 *
				 * set the vblank- identifier as part of the exported planeset
				 * then TAG the signal with SHMIF_SIGVID_RELEASE_FEEDBACK
				 *
				 * and we can release-chain the buffer on that.
				 */
#endif
        uintptr_t adisp, actx;
        arcan_shmifext_egl_meta(C, &adisp, NULL, &actx);
        arcan_shmifext_signal(C,
                              adisp,
                              SHMIF_SIGVID | SHMIF_SIGBLK_NONE,
                              apmap->texture);
    }
    else {
        fprintf(stderr, "missing, present on non-gl");
    }

/* this should extract the shmif- reference to the pixmap,
 * translate damage into dirty
 * signal the transfer */
    return TRUE;
}

/*
 * Once the required MSC has been reached, execute the pending request.
 *
 * For requests to actually present something, either blt contents to
 * the window pixmap or queue a window buffer swap on the backend.
 *
 * For requests to just get the current MSC/UST combo, skip that part and
 * go straight to event delivery.
 */
static void
xa_present_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr               window = vblank->window;
    struct xa_present_window *xa_present_window = xa_present_window_get_priv(window);
    present_vblank_ptr flip_pending = xa_present_get_pending_flip(xa_present_window);

    xorg_list_del(&vblank->event_queue);

    if (present_execute_wait(vblank, crtc_msc))
        return;

    if (flip_pending && vblank->flip && vblank->pixmap && vblank->window) {
        DebugPresent(("\ttype=exec_pending:id=%" PRIu64 ":ptr=%p:pending_ptr=%p\n",
                      vblank->event_id, vblank, flip_pending));
        xorg_list_append(&vblank->event_queue, &xa_present_window->flip_queue);
        vblank->flip_ready = TRUE;
        return;
    }

    vblank->queued = FALSE;

    if (vblank->pixmap && vblank->window) {
        ScreenPtr screen = window->drawable.pScreen;

        if (vblank->flip) {
            RegionPtr damage;

            DebugPresent(("\ttype=exec_flip:id=%" PRIu64 ":ptr=%p:msc=%" PRIu64 ":pmap=%08" PRIx32 ":wnd=%08" PRIx32 "\n",
                          vblank->event_id, vblank, crtc_msc,
                          vblank->pixmap->drawable.id, vblank->window->drawable.id));

            /* Set update region as damaged */
            if (vblank->update) {
                damage = RegionDuplicate(vblank->update);
                /* Translate update region to screen space */
                assert(vblank->x_off == 0 && vblank->y_off == 0);
                RegionTranslate(damage, window->drawable.x, window->drawable.y);
                RegionIntersect(damage, damage, &window->clipList);
            } else
                damage = RegionDuplicate(&window->clipList);

            if (xa_present_flip(vblank, damage)) {
                WindowPtr toplvl_window = xa_present_toplvl_pixmap_window(vblank->window);
                PixmapPtr old_pixmap = screen->GetWindowPixmap(window);

                /* Replace window pixmap with flip pixmap */
#ifdef COMPOSITE
                vblank->pixmap->screen_x = old_pixmap->screen_x;
                vblank->pixmap->screen_y = old_pixmap->screen_y;
#endif
                present_set_tree_pixmap(toplvl_window, old_pixmap, vblank->pixmap);
                vblank->pixmap->refcnt++;
                dixDestroyPixmap(old_pixmap, old_pixmap->drawable.id);

                /* Report damage */
                DamageDamageRegion(&vblank->window->drawable, damage);
                RegionDestroy(damage);

                /* Put pending flip at the flip queue head */
                xorg_list_add(&vblank->event_queue, &xa_present_window->flip_queue);

                return;
            }

            vblank->flip = FALSE;
        }

        DebugPresent(("\ttype=flip_over:ptr=%p:msc=%" PRIu64 ":pmap=%08" PRIx32 ":wnd=%08" PRIx32 "\n",
                      vblank, crtc_msc, vblank->pixmap->drawable.id, vblank->window->drawable.id));

        if (flip_pending)
            flip_pending->abort_flip = TRUE;
        else if (xa_present_window->flip_active)
            xa_present_flips_stop(window);

        present_execute_copy(vblank, crtc_msc);
        assert(!vblank->queued);

        /* Clear the pixmap field, so this will fall through to present_execute_post next time */
        dixDestroyPixmap(vblank->pixmap, vblank->pixmap->drawable.id);
        vblank->pixmap = NULL;

        if (xa_present_queue_vblank(screen, window, vblank->crtc,
                                     vblank->event_id, crtc_msc + 1)
            == Success)
            return;
    }

    present_execute_post(vblank, ust, crtc_msc);
}

static int
xa_present_pixmap(WindowPtr window,
                   PixmapPtr pixmap,
                   CARD32 serial,
                   RegionPtr valid,
                   RegionPtr update,
                   int16_t x_off,
                   int16_t y_off,
                   RRCrtcPtr target_crtc,
                   SyncFence *wait_fence,
                   SyncFence *idle_fence,
                   uint32_t options,
                   uint64_t target_window_msc,
                   uint64_t divisor,
                   uint64_t remainder,
                   present_notify_ptr notifies,
                   int num_notifies)
{
    static uint64_t xwl_present_event_id;
    uint64_t                    ust = 0;
    uint64_t                    target_msc;
    uint64_t                    crtc_msc = 0;
    int                         ret;
    present_vblank_ptr          vblank, tmp;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_get_window_priv(window, TRUE);
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    struct xa_present_event *event;

    if (!window_priv)
        return BadAlloc;

    target_crtc = xa_present_get_crtc(screen_priv, window);

    ret = xa_present_get_ust_msc(screen, window, &ust, &crtc_msc);

    xa_present_update_window_crtc(window_priv, target_crtc, crtc_msc);

    if (ret == Success) {
        /* Stash the current MSC away in case we need it later
         */
        window_priv->msc = crtc_msc;
    }

    target_msc = present_get_target_msc(target_window_msc + window_priv->msc_offset,
                                        crtc_msc,
                                        divisor,
                                        remainder,
                                        options);

    /*
     * Look for a matching presentation already on the list...
     */

    if (!update && pixmap) {
        xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->vblank, window_list) {

            if (!vblank->pixmap)
                continue;

            if (!vblank->queued)
                continue;

            if (vblank->target_msc != target_msc)
                continue;

            present_vblank_scrap(vblank);
            if (vblank->flip_ready)
                xa_present_re_execute(vblank);
        }
    }

    event = calloc(1, sizeof(*event));
    if (!event)
        return BadAlloc;

    vblank = &event->vblank;
    if (!present_vblank_init(vblank, window, pixmap, serial, valid, update, x_off, y_off,
                             target_crtc, wait_fence, idle_fence, options, XA_PRESENT_CAPS,
                             notifies, num_notifies, target_msc, crtc_msc)) {
        present_vblank_destroy(vblank);
        return BadAlloc;
    }

    vblank->event_id = ++xwl_present_event_id;
    event->async_may_tear = options & PresentOptionAsyncMayTear;

    /* Synchronous Xwayland presentations always complete (at least) one frame after they
     * are executed
     */
    if (event->async_may_tear)
        vblank->exec_msc = vblank->target_msc;
    else
        vblank->exec_msc = vblank->target_msc - 1;

    vblank->queued = TRUE;
    if (crtc_msc < vblank->exec_msc) {
        if (xa_present_queue_vblank(screen, window, target_crtc, vblank->event_id, vblank->exec_msc) == Success)
            return Success;

        DebugPresent(("type=queue_vblank_fail\n"));
    }

    xa_present_execute(vblank, ust, crtc_msc);
    return Success;
}

void
xa_present_unrealize_window(struct xa_present_window *xa_present_window)
{
/* Make sure the timer callback doesn't get called
 * ignore for now as our times are tied to the arcan-shmif-window
    xwl_present_window->timer_armed = 0;
    xwl_present_reset_timer(xwl_present_window);
*/
}

Bool
xa_present_init(ScreenPtr screen)
{
    present_screen_priv_ptr screen_priv;

    /* xwl checks for this here, we do it outside
     * struct xwl_screen *xwl_screen = xwl_screen_get(screen);
       if (!xwl_screen->glamor || !xwl_screen->egl_backend)
           return FALSE;
     */

    if (!present_screen_register_priv_keys())
        return FALSE;

    if (present_screen_priv(screen))
        return TRUE;

    screen_priv = present_screen_priv_init(screen);
    if (!screen_priv)
        return FALSE;

    if (!dixRegisterPrivateKey(&xa_present_window_private_key, PRIVATE_WINDOW, 0))
        return FALSE;

    screen_priv->query_capabilities = xa_present_query_capabilities;
    screen_priv->get_crtc = xa_present_get_crtc;

    screen_priv->check_flip = xa_present_check_flip;
    screen_priv->check_flip_window = xa_present_check_flip_window;
    screen_priv->clear_window_flip = xa_present_clear_window_flip;

    screen_priv->present_pixmap = xa_present_pixmap;
    screen_priv->queue_vblank = xa_present_queue_vblank;
    screen_priv->flush = xa_present_flush;
    screen_priv->re_execute = xa_present_re_execute;

    screen_priv->abort_vblank = xa_present_abort_vblank;

    return TRUE;
}
