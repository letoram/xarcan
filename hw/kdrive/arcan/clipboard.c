/* this should be moved to some single- header structure so that
 * we can just re-use the thing for aclip */
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

_Thread_local static xcb_connection_t *xcon;
_Thread_local static xcb_window_t xwnd;
_Thread_local static FILE* selectionSink;
_Thread_local static bool incrState;

_Thread_local static struct
{
    xcb_atom_t clipboard;
    xcb_atom_t local;
    xcb_atom_t utf8;
    xcb_atom_t compound;
    xcb_atom_t targets;
    xcb_atom_t incr;
    xcb_atom_t data;
} atoms;

static xcb_atom_t lookupAtom(const char *name)
{
  xcb_intern_atom_reply_t *atom_reply;
  xcb_intern_atom_cookie_t atom_cookie;
  xcb_atom_t atom = XCB_ATOM_NONE;

  atom_cookie = xcb_intern_atom(xcon, 0, strlen(name), name);
  atom_reply = xcb_intern_atom_reply(xcon, atom_cookie, NULL);
  if (atom_reply) {
    atom = atom_reply->atom;
    free(atom_reply);
  }
  return atom;
}

static void setupAtoms(void)
{
    atoms.clipboard = lookupAtom("CLIPBOARD");
    atoms.utf8 = lookupAtom("UTF8_STRING");
    atoms.compound = lookupAtom("COMPOUND_TEXT");
    atoms.targets = lookupAtom("TARGETS");
    atoms.incr = lookupAtom("INCR");
    atoms.data = lookupAtom("XSEL_DATA");
}

static char *atomName(xcb_atom_t atom)
{
    char *ret;
    xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(xcon, atom);
    xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(xcon, cookie, NULL);
    if (!reply)
        return NULL;

    ret = malloc(xcb_get_atom_name_name_length(reply) + 1);
    if (ret) {
        memcpy(ret, xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
        ret[xcb_get_atom_name_name_length(reply)] = '\0';
    }
    free(reply);
    return ret;
}

static bool buildWindow(struct proxyWindowData* inWnd)
{
    xcon = xcb_connect_to_fd(inWnd->socket, NULL);

    if (xcb_connection_has_error(xcon)){
        free(inWnd);
        return false;
    }

    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(xcon)).data;


    xwnd = xcb_generate_id(xcon);
    xcb_void_cookie_t cookie =
                      xcb_create_window_checked(
                          xcon,
                          XCB_COPY_FROM_PARENT,
                          xwnd, screen->root,
                          1, 1, 32, 32,
                          0,
                          XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT,
                          0,
                          NULL
                      );

    xcb_icccm_set_wm_name(xcon, xwnd, XCB_ATOM_STRING,
                          8, sizeof("arcan-clip")-1, "arcan-clip");

        static const uint32_t propchg[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    cookie = xcb_change_window_attributes_checked(xcon, xwnd, XCB_CW_EVENT_MASK, propchg);
    xcb_generic_error_t *error = xcb_request_check(xcon, cookie);
    if (error){
        ErrorF("Xarcan clipboard: could not build/setup integration");
        free(error);
        return false;
    }
     return true;
}

/*
 * static void getSelection(struct proxyWindowData* inWnd)
{*/
/*
 * xcb_convert_selection(globalconf.connection, selection_window,
                          XCB_ATOM_PRIMARY, UTF8_STRING, XSEL_DATA, globalconf.timestamp);
    xcb_flush(globalconf.connection);
 */
/* missing: also watch for clipboard but we need to resolve that atom
   cookie = xcb_set_selection_owner_checked(conn, window, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);

     the selection is released when the context is destroyed
 */
/*
}
*/

/*
static void requestTargets(xcb_atom_t selection)
{
*/
/* With many clients we can only really do the content transfer when there is an actual
 * standing interactive request. We need a special 'probe' type to request the full set,
 * otherwise fall back on a 'safe' utf8 */
/*
    xcb_convert_selection(xcon, xwnd, selection, atoms.targets, atoms.data, XCB_CURRENT_TIME);
*/

static void setOwnership(void)
{
    xcb_set_selection_owner_checked(xcon, xwnd, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
        xcb_set_selection_owner_checked(xcon, xwnd, atoms.clipboard, XCB_CURRENT_TIME);
}

static void watchForNew(struct proxyWindowData* inWnd)
{
    xcb_xfixes_select_selection_input(
        xcon,
        xwnd,
        XCB_ATOM_PRIMARY,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE
    );

    xcb_xfixes_select_selection_input(
        xcon,
        xwnd,
        atoms.clipboard,
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE
    );
}

_Thread_local char* currentMessage = NULL;
static void updateSelection(struct arcan_event* aev)
{
/* deal with multipart vs. split first before we commit something new */
}

static void xfixesSelectionNotify(
    struct proxyWindowData* inWnd, xcb_xfixes_selection_notify_event_t* event)
{
        struct arcan_event out = {
            .category = EVENT_EXTERNAL,
            .ext.kind = ARCAN_EVENT(MESSAGE)
        };
        size_t msglen = sizeof(out.ext.message.data) / sizeof(out.ext.message.data[0]);
        snprintf((char*)out.ext.message.data, msglen, "xfixesNotify");
        arcan_shmif_enqueue(inWnd->cont, &out);

/* 1. selection as CLIPBOARD_MANAGER?,
 * 2. ATOM_TARGETS:
 *    ATOM_TIMESTAMP, ATOM_TARGETS, ATOM_UTF8_STRING, ATOM_TEXT
 *    xcb_change_property(xcon, XCP_PROP_MODE_REPLACE,
 *    requestor, property, XCB_ATOM_ATOM, 32, ARRAY_SIZE(targets), targets)
 * 3. send_notify:
 *    .sequence = 0, .time = request.time, .requestor, .selection, .target, property,
 *    xcb_send_event()
 */
}

static void selectionRequest(
    struct proxyWindowData* inWnd, xcb_selection_request_event_t* event)
{
    if (event->target != XCB_ATOM_STRING &&
            event->target != atoms.utf8 &&
            event->target != atoms.targets)
        return;


/* If we hold the selection, and someone requests our 'targets', then provide
 * the list of what we know we can handle. This will always be UTF8 + any other
 * custom ones that has been provided as messages on the clipboard */
    if (event->target == atoms.targets){
        xcb_atom_t types[] = { atoms.targets, atoms.utf8, XCB_ATOM_STRING };
        xcb_change_property_checked(xcon, XCB_PROP_MODE_REPLACE,
            event->requestor, event->property, XCB_ATOM_ATOM, 32, ARRAY_SIZE(types), (unsigned char*) types);
        xcb_selection_notify_event_t selection = {
            .response_type = XCB_SELECTION_NOTIFY,
            .requestor = event->requestor,
            .selection = event->selection,
            .target = event->target,
            .property = event->property,
            .time = event->time
        };
        xcb_send_event_checked(xcon, FALSE, event->requestor, 0, (char *)&selection);
        return;
    }

/* if we are here it means that a window should have a copy of our string or
 * other selected target type */
    const char* placeholder = "hi there this is a test";
    size_t placeholder_sz = sizeof(placeholder);
    uint32_t len = xcb_get_maximum_request_length(xcon);
    if (placeholder_sz > len){
        ErrorF("clipboard item size exceeds limit\n");
        return;
    }

    xcb_change_property_checked(xcon, XCB_PROP_MODE_REPLACE,
            event->requestor, event->property, event->target, 8, placeholder_sz, placeholder);
    xcb_send_event_checked(xcon, FALSE, event->requestor, 0,
        (char*)&(struct xcb_selection_notify_event_t){
            .response_type = XCB_SELECTION_NOTIFY,
            .requestor = event->requestor,
            .selection = event->selection,
            .target = event->target,
            .property = event->property,
            .time = event->time
        }
    );
}

static void forwardData(struct proxyWindowData *inWnd,
                        xcb_get_property_reply_t *reply, bool oneshot)
{
    if (!selectionSink){
      ErrorF("Xarcan clipboard: selection data without sink");
    }

    void* src = xcb_get_property_value(reply);
    size_t len = xcb_get_property_value_length(reply);

    if (len)
        fwrite(src, len, 1, selectionSink);

    if (!len || oneshot){
        fclose(selectionSink);
        selectionSink = NULL;
    }

    free(reply);
}

static void forwardTargets(struct proxyWindowData *inWnd,
                           xcb_selection_notify_event_t *event)
{
    xcb_get_property_cookie_t cookie;
    cookie = xcb_get_property(xcon,
                              TRUE, xwnd, atoms.local,
                              XCB_GET_PROPERTY_TYPE_ANY, 0, INT_MAX);

    xcb_get_property_reply_t *reply = xcb_get_property_reply(xcon, cookie, NULL);
    if (!reply)
        return;

    /* Package each possible output type as a BCHUNKHINT, normally these
     * replace eachother - but here we reset with an empty one then accumulate
     * due to the possible large set of types. Normally copy and paste are two
     * different, but here we just map to UTF8 or binary blobs based on the
     * hint type. */
    struct arcan_event out = {
        .category = EVENT_EXTERNAL,
        .ext.kind = ARCAN_EVENT(BCHUNKSTATE)
    };

    xcb_atom_t *pval = xcb_get_property_value(reply);
    int n = xcb_get_property_value_length(reply) / sizeof(xcb_atom_t);

    for (int i = 0; i < n; i++){
        xcb_atom_t atom = pval[i];
        char *name = atomName(atom);
        snprintf((char*)out.ext.bchunk.extensions, sizeof(out.ext.message), "prop:%s", name);
        arcan_shmif_enqueue(inWnd->cont, &out);
    }

    free(reply);
}

static void propertyNotify(
		struct proxyWindowData *inWnd, xcb_property_notify_event_t * event)
{

}

static void selectionNotify(
    struct proxyWindowData* inWnd, xcb_selection_notify_event_t* event)
{
    if (event->selection != atoms.clipboard && event->selection != XCB_ATOM_PRIMARY)
        return;

/* this is for the exposed set of targets, i.e. which types can the source of
 * the selection handle conversion to. */
    if (event->target == atoms.targets){
        forwardTargets(inWnd, event);
            return;
    }

    xcb_get_property_cookie_t cookie;
    cookie = xcb_get_property(xcon,
                              TRUE, xwnd,
                              event->target,
                              XCB_GET_PROPERTY_TYPE_ANY,
                              0,
                              0x1fffffff);

    xcb_get_property_reply_t *reply = xcb_get_property_reply(xcon, cookie, NULL);

    if (!reply)
        return;

    if (reply->type == atoms.incr){
      incrState = true;
      free(reply);
      return;
    }

    forwardData(inWnd, reply, true);
}

void *arcanClipboardDispatch(struct proxyWindowData* inWnd, bool paste)
{
    if (!buildWindow(inWnd)){
                arcan_shmif_drop(inWnd->cont);
                free(inWnd);
                return NULL;
        }

    setupAtoms();

  xcb_void_cookie_t cookie =
        xcb_set_selection_owner_checked(xcon, xwnd, atoms.clipboard, XCB_CURRENT_TIME);

    xcb_prefetch_extension_data(xcon, &xcb_xfixes_id);
    const xcb_query_extension_reply_t *xfixes_id = xcb_get_extension_data(xcon, &xcb_xfixes_id);

    xcb_generic_error_t *error;
  if ((error = xcb_request_check(xcon, cookie))){
        goto out;
    }

    watchForNew(inWnd);
  xcb_generic_event_t *event;

/* need to poll on both the shmif context and the xcb descriptor, as even for
 * 'paste' the xcb response need to be snappy in order to forward the selection
 * - we are just another client */
    struct pollfd pset[2] = {
        {
            .fd = xcb_get_file_descriptor(xcon),
            .events = POLLIN | POLLHUP | POLLERR
        },
        {
            .fd = inWnd->cont->epipe,
            .events = POLLIN | POLLHUP | POLLERR
        },
    };

    setOwnership();

    while (1){
        xcb_flush(xcon);
        if (-1 == poll(pset, 2, -1)){
            if (errno == EINTR)
                continue;
            else
                break;
        }

    /* select for data on the arcan and other socket */
    /* poll context and look for inbound messages or data streams */
        struct arcan_event aev;
        int pv;

        while ( (pv = arcan_shmif_poll(inWnd->cont, &aev) > 0 )){
            if (aev.category != EVENT_TARGET)
                continue;

    /* The model in arcan clipboard is 'push' which is more similar to how a clipboard
     * manager would work, e.g. every time the selection owner changes - copy that and
     * forward. We should have that over bchunk in order to have larger transfer sets. */
            switch (aev.tgt.kind){
                case TARGET_COMMAND_MESSAGE:
                    updateSelection(&aev);
                break;
                case TARGET_COMMAND_BCHUNK_IN:
                break;

                case TARGET_COMMAND_BCHUNK_OUT:{
								    xcb_atom_t selection = XCB_ATOM_PRIMARY;
										xcb_atom_t target = atoms.utf8;

								    if (selectionSink) /* only permit one ongoing transfer, this is racey */
                        fclose(selectionSink);
                    selectionSink = fdopen(arcan_shmif_dupfd(aev.tgt.ioevs[0].iv, -1, true), "w");

										size_t offset = 0;
										if (strncmp(aev.tgt.message, "primary", 7) == 0){
											offset = 7;
										}
										else if (strncmp(aev.tgt.message, "clipboard", 9) == 0){
											offset = 9;
										}

										/* pick the desired selection type, or if we are requesting a probe, start with that */
										if (strcmp(&aev.tgt.message[offset], ":probe") == 0){
											target = atoms.targets;
										}

										xcb_convert_selection(xcon, xwnd, target, selection, atoms.data, XCB_CURRENT_TIME);
								}
                break;
                case TARGET_COMMAND_EXIT:
                    goto out;
                break;
                default:
                break;
            }
        }

        struct arcan_event out = {
            .category = EVENT_EXTERNAL,
            .ext.kind = ARCAN_EVENT(MESSAGE)
        };
        size_t msglen = sizeof(out.ext.message.data) / sizeof(out.ext.message.data[0]);

        while ((event = xcb_poll_for_event(xcon))){
            snprintf((char*)out.ext.message.data, msglen, "xcb-%s", xcb_event_get_label(event->response_type));
            arcan_shmif_enqueue(inWnd->cont, &out);

            event->response_type &= ~0x80;
            switch (event->response_type){
             case XCB_SELECTION_REQUEST:
               selectionRequest(inWnd, (xcb_selection_request_event_t *) event);
             break;
             case XCB_SELECTION_NOTIFY:
               selectionNotify(inWnd, (xcb_selection_notify_event_t *) event);
             break;
             case XCB_SELECTION_CLEAR:
/* this happens when we have lost the selection to someone else, to get clipboard manager
 * like behavior we should take the selection back and retrieve it from who owns it, but
 * that is problematic when there are multiples .. */
             break;
             case XCB_PROPERTY_NOTIFY:
						     propertyNotify(inWnd, (xcb_property_notify_event_t *) event);
             break;
             default:
             break;
            }
            switch (event->response_type - xfixes_id->first_event){
            case XCB_XFIXES_SELECTION_NOTIFY:
                xfixesSelectionNotify(inWnd, (xcb_xfixes_selection_notify_event_t *)event);
            break;
            default:
            break;
            }
        }
    }


out:
        arcan_shmif_drop(inWnd->cont);

        if (xwnd){
            xcb_destroy_window(xcon, xwnd);
            xwnd = 0;
        }

        if (xcon)
        xcb_disconnect(xcon);

        free(inWnd);
        return NULL;
}
