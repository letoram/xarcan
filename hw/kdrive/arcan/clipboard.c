#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "arcan.h"
#include <inttypes.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xfixes.h>

void *arcanClipboardDispatch(struct arcan_shmif_cont* clip)
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
                          1, 1, 32, 32,
                          0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT,
													0,
													NULL
                     );

    xcb_generic_error_t *error;
    if ((error = xcb_request_check(conn, cookie))) {
        ErrorF("winClipboardProc - Could not create an X window.\n");
        free(error);
        goto winClipboardProc_Done;
    }

		xcb_icccm_set_wm_name(conn, XCB_ATOM_STRING, 8, strlen("arcan-clip"), "arcan-clip");

		static const uint32_t propchg[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    cookie = xcb_change_window_attributes_checked(conn, window, XCB_CW_EVENT_MASK, propchg);
    if ((error = xcb_request_check(conn, cookie))) {
        ErrorF("Xarcan clipboard: could not build/setup integration");
        free(error);
				goto out;
    }

		xcb_xfixes_select_selection_input(conn,
                                      window,
                                      XCB_ATOM_PRIMARY,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);

    xcb_xfixes_select_selection_input(conn,
                                      window,
                                      atoms.atomClipboard,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);

		cookie = xcb_set_selection_owner_checked(conn, iWindow, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    cookie = xcb_set_selection_owner_checked(conn, iWindow, atoms.atomClipboard, XCB_CURRENT_TIME);

		while (1){
	/* select for data on the arcan and other socket */
	/* poll context and look for inbound messages or data streams */
		    while ((event = xcb_poll_for_event(conn))){
				   switch (event->response_type & ~0x80){
					 case XCB_SELECTION_REQUEST:
					     forward_selection_request(clip);
					 break;
					 }
				}

				while ( arcan_shmif_poll(clip, &aev) > 0){
					switch(ev.tgt.kind){
 			    case TARGET_COMMAND_EXIT:
					    goto out;
					break;
					case TARGET_COMMAND_MESSAGE:
					break;
					case TARGET_COMMAND_BCHUNK_IN:
					break;
					case TARGET_COMMAND_BCHUNK_OUT:
					break;
					}
				}
		}

out:
		arcan_shmif_drop(clip);
    xcb_disconnect(con);

		return NULL;
}
