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
    values[0] = screen->white_pixel;
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
    values[0] = screen->white_pixel;
    values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS;

    xcb_window_t window = xcb_generate_id(con);
    xcb_create_window_checked(con,
                          XCB_COPY_FROM_PARENT,
                          window, screen->root,
                          0, 0,
                          inWnd->cont->w, inWnd->cont->h,
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask, values
                     );

/* we are not typically supposed to access the shmif mmap descriptor, but doing
 * so allows us to forward it back into the main thread, have the regular Xorg
 * window management bind it to a PIXMAP and we can specify the vidp offset.
 */
    xcb_pixmap_t pixmap = xcb_generate_id(con);
    xcb_shm_seg_t segment = xcb_generate_id(con);

/* A few optimizations still missing here, one is wiring dma-buf back into dri3
 * if the context support handle passing. That would be through
 * xcb_dri3_pixmap_from_buffers(xcb, pixmap, window, planes, w, h,
 *  stride0, offset0, .., depth, bpp, fd) and the rest should sort itself.
 *
 * the other is marking ourselves as double-buffered and then step the offset
 * on STEPFRAME. This is also an opportunity to inject some virtual audio
 * device that forwards the audio buffer coming from the segment. */
    xcb_shm_attach_fd_checked(con, inWnd->cont->shmh, segment, 1);
    xcb_shm_create_pixmap_checked(con,
                          pixmap,
                          window,
                          inWnd->cont->w,
                          inWnd->cont->h,
                          4,
                          segment,
                          (uintptr_t) inWnd->cont->vidp - (uintptr_t) inWnd->cont->addr);

/*  xcb_shm_detach */

    xcb_gcontext_t gc = xcb_generate_id(con);
    xcb_create_gc_checked(con, gc, window, 0, 0);
    xcb_map_window_checked(con, window);

/* There is not many events we actually need to think about here as
 * the window synch process does most of it for us (focus, unfocus, ...) */
    struct pollfd fds[] =
    {
        {.fd = inWnd->socket,      POLLIN | POLLERR | POLLHUP},
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
                  gc, 0, 0, 0, 0, inWnd->cont->w, inWnd->cont->h);
            }
        }
      }
        if (fds[1].revents){
            if (fds[1].revents & (POLLERR | POLLHUP))
                break;

        struct arcan_event ev;
        while (arcan_shmif_poll(inWnd->cont, &ev) > 0){
/* on stepframe, xcb_copy, flush and then signal */
            }
        }
    }
    arcan_shmif_drop(inWnd->cont);

    xcb_disconnect(con);
    free(inWnd);
    return NULL;
}
