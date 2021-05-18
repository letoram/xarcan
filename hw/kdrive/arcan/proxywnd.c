#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "arcan.h"
#include <inttypes.h>
#include <xcb/xcb.h>

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
