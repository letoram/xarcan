#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "arcan.h"
#include <inttypes.h>
#include <poll.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_image.h>

/* as with clipboard.c - just modifying internal Xorg structures without going
 * through the protocol itself should be the better path, but when tried there
 * was always some edge condition / structure that did not get filled out
 * correctly, found very few examples of Xorg creating 'fake' windows within
 * itself other than some special ones (e.g. root) */

void *arcanProxyWindowDispatch(struct proxyWindowData* inWnd)
{
    xcb_connection_t *con = xcb_connect_to_fd(inWnd->socket, NULL);

    if (xcb_connection_has_error(con)){
        free(inWnd);
        return NULL;
    }

    uint32_t mask;
    uint32_t values[4];

    mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    xcb_screen_t *screen =
                 xcb_setup_roots_iterator(xcb_get_setup(con)).data;
    values[0] = screen->black_pixel;
    values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS;

    xcb_window_t window = xcb_generate_id(con);
    xcb_create_window(con,
                          screen->root_depth,
                          window, screen->root,
                          inWnd->x, inWnd->y,
                          inWnd->w, inWnd->h,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask, values
                     );
    struct arcan_event arcan_ev = (struct arcan_event){
                                      .ext.kind = ARCAN_EVENT(MESSAGE)
                                  };

    arcanScrPriv *scrpriv = inWnd->cont->user;
    snprintf((char*)arcan_ev.ext.message.data, 78,
             "kind=pair:xid=%"PRIu32":vid=%"PRIu32,
             (uint32_t)window, inWnd->arcan_vid
            );

    arcan_shmif_lock(inWnd->cont);
        struct proxyMapEntry *ent = ht_add(scrpriv->proxyMap, &window);
        ent->vid = inWnd->arcan_vid;
        arcan_shmif_enqueue(inWnd->cont, &arcan_ev);
    arcan_shmif_unlock(inWnd->cont);

    xcb_map_window(con, window);
    xcb_intern_atom_cookie_t iac_prot = xcb_intern_atom(con, 1, 12, "WM_PROTOCOLS");
    xcb_intern_atom_reply_t *rep_prot = xcb_intern_atom_reply(con, iac_prot, 0);
    xcb_intern_atom_cookie_t iac_close = xcb_intern_atom(con, 1, 16, "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t *rep_close = xcb_intern_atom_reply(con, iac_close, 0);
    xcb_change_property(con, XCB_PROP_MODE_REPLACE,
                             window, rep_prot->atom, 4, 32, 1, &rep_close->atom);

/* There is not many events we actually need to think about here as
 * the window synch process does most of it for us (focus, unfocus, ...) */

     while (xcb_flush(con) > 0){
        xcb_generic_event_t* event = xcb_wait_for_event(con);
        if (event){
            switch((*event).response_type & ~0x80){
            case XCB_CLIENT_MESSAGE:{
              xcb_client_message_event_t *msg = (xcb_client_message_event_t*) event;
              if (msg->data.data32[0] == rep_close->atom){
                  goto out;
              }
            break;
            }
            }
        }
    }

out:
    arcan_shmif_lock(inWnd->cont);
        ht_remove(scrpriv->proxyMap, &window);
    arcan_shmif_unlock(inWnd->cont);
    xcb_disconnect(con);
    free(inWnd);
    return NULL;
}

/* This has a similar style to the ProxyWindow, but we have a separate OUTPUT
 * segment to pair to. Instead of pairing and letting Arcan composite it, Arcan
 * fills it with data in order to satisfy a sharing condition where we don't
 * want to swap out and protect a specific Pixmap. */
void *arcanProxyContentWindowDispatch(struct proxyWindowData* inWnd)
{
    xcb_connection_t *con = xcb_connect_to_fd(inWnd->socket, NULL);

    if (xcb_connection_has_error(con)){
        free(inWnd);
        return NULL;
    }

    uint32_t mask;
    uint32_t values[4];

    mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    xcb_screen_t *screen =
                 xcb_setup_roots_iterator(xcb_get_setup(con)).data;
    values[0] = screen->black_pixel;
    values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS;

    xcb_window_t window = xcb_generate_id(con);
    xcb_create_window(con,
                          XCB_COPY_FROM_PARENT,
                          window, screen->root,
                          inWnd->x, inWnd->y,
                          inWnd->w, inWnd->h,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask, values
                     );

    values[0] = screen->black_pixel;
    values[1] = 0;
    xcb_gcontext_t gc = xcb_generate_id(con);
    xcb_create_gc(con,
                  gc, window,
                  XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES, values);

/* we are not typically supposed to access the shmif mmap descriptor, but doing
 * so allows us to forward it back into the main thread, have the regular Xorg
 * window management bind it to a PIXMAP and we can specify the vidp offset.
 */
    xcb_map_window(con, window);
    xcb_flush(con);

    xcb_shm_query_version_reply(con, xcb_shm_query_version(con), NULL);

    xcb_pixmap_t pixmap = xcb_generate_id(con);
    xcb_shm_seg_t segment = xcb_generate_id(con);

    int fd = inWnd->cont->shmh;
    uintptr_t ofs = (uintptr_t) inWnd->cont->vidp - (uintptr_t) inWnd->cont->addr;

/* A few optimizations still missing here, one is wiring dma-buf back into dri3
 * if the context support handle passing. That would be through
 * xcb_dri3_pixmap_from_buffers(xcb, pixmap, window, planes, w, h,
 *  stride0, offset0, .., depth, bpp, fd) and the rest should sort itself.
 *
 * the other is marking ourselves as double-buffered and then step the offset
 * on STEPFRAME. This is also an opportunity to inject some virtual audio
 * device that forwards the audio buffer coming from the segment.
 *
 * Marking this as read-only will actually have the pixmap creation fail
 */
    xcb_shm_attach_fd(con, segment, fd, 0);
    xcb_shm_create_pixmap(con,
                          pixmap,
                          window,
                          inWnd->w, inWnd->h, screen->root_depth,
                          segment, ofs);

    xcb_copy_area(con, pixmap, window,
                  gc, 0, 0, 0, 0, inWnd->w, inWnd->h);

    xcb_flush(con);

/* There is not many events we actually need to think about here as
 * the window synch process does most of it for us (focus, unfocus, ...) */
    struct pollfd fds[] =
    {
        {.fd = inWnd->socket, POLLIN | POLLERR | POLLHUP},
        {.fd = inWnd->cont->epipe, POLLIN | POLLERR | POLLHUP}
    };

    xcb_flush(con);

    while (-1 != poll(fds, 2, -1)){
        if (fds[0].revents){
            if (fds[0].revents & (POLLERR | POLLHUP))
                break;

            xcb_generic_event_t* ev;
            while ((ev = xcb_poll_for_event(con))){
                switch ((*ev).response_type & ~0x80){
                case XCB_EXPOSE:
                      xcb_copy_area(con, pixmap, window,
                                    gc, 0, 0, 0, 0, inWnd->w, inWnd->h);
                      xcb_flush(con);
                break;
                }
            }
        }
        if (fds[1].revents){
            if (fds[1].revents & (POLLERR | POLLHUP))
                break;

            struct arcan_event ev;
            while (arcan_shmif_poll(inWnd->cont, &ev) > 0){
                if (ev.category == EVENT_TARGET &&
                    ev.tgt.kind == TARGET_COMMAND_STEPFRAME){
                    xcb_copy_area(con,
                                  pixmap, window, gc,
                                  0, 0, 0, 0,
                                  inWnd->cont->w, inWnd->cont->h);
                    xcb_flush(con);
/*
 * This lacks PRESENT feedback in order to safely STEPFRAME unless we are
 * n-buffered, which is out of our control. Other question is what we are
 * supposed to do with any audio (if present).
 * This would be the opportunity to shove things back into some virtual source.
 */
                    arcan_shmif_signal(inWnd->cont, SHMIF_SIGVID);
                }
            }
        }
    }
    arcan_shmif_drop(inWnd->cont);

    xcb_disconnect(con);
    free(inWnd);
    return NULL;
}
