/* This actually spins up a separate xorg connection internally as a thread and
 * then using xcb to do all the clipboard translation in a similar vein as a
 * clipboard 'manager' would do. This was to test/reuse the same code as part
 * of other tooling (arcan-wayland -x11) and so on, and an effect of a sour
 * experience trying to build a fake client inside Xorg (FakeClientID, ...).
 *
 * Splitting off the interface and going with XaceHookSelectionAccess might
 * be another possibility and patch ourselves in there using:
 * XaceRegisterCallback(XACE_SELECTION_ACCESS, ..., NULL)
 *
 * There might be good reason as to why Xwin and so on did not go that route
 * but couldn't find anything obvious in the logs. The sink end of drag and
 * drop is only needed for the Arcan window itself ('kindof'), though it would
 * be nice to intercept it in some cases (window redirection).
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

/* trace to file as to not fight the other threads */
#ifdef DEBUG
#define ARCAN_TRACE
#endif

_Thread_local static FILE* logout;
static inline void trace(const char* msg, ...)
{
#ifdef ARCAN_TRACE
    if (!logout){
        logout = fopen("clipboard.log", "w+");
    }
    va_list args;
    va_start( args, msg );
        vfprintf(logout, msg, args );
        fprintf(logout, "\n");
    va_end( args);
    fflush(logout);
#endif
}

_Thread_local static xcb_connection_t *xcon;
_Thread_local static xcb_window_t xwnd;
_Thread_local static struct
{
    FILE* sink;
    size_t len;
    char* buf;
} clip_out;

_Thread_local static bool incrState;
_Thread_local static struct
{
    size_t len;
    size_t buffer_sz;
    char* buffer;
    bool done;
} clip_in;

_Thread_local static struct
{
    xcb_atom_t clipboard;
    xcb_atom_t clipboard_mgr;
    xcb_atom_t local;
    xcb_atom_t utf8;
    xcb_atom_t compound;
    xcb_atom_t targets;
    xcb_atom_t incr;
    xcb_atom_t data;
    xcb_atom_t selection_id;
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
    atoms.selection_id = lookupAtom("_ARCAN_SELECTION");
    atoms.clipboard_mgr = lookupAtom("CLIPBOARD_MANAGER");
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

    xcb_screen_t *screen = xcb_aux_get_screen(xcon, 0);
    xwnd = xcb_generate_id(xcon);

    xcb_void_cookie_t cookie =
                      xcb_create_window(
                          xcon,
                          screen->root_depth,
                          xwnd, screen->root,
                          -1, -1, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          XCB_CW_EVENT_MASK,
                          (uint32_t[1]){XCB_EVENT_MASK_PROPERTY_CHANGE}
                      );

    xcb_generic_error_t *error = xcb_request_check(xcon, cookie);
    if (error){
        ErrorF("Xarcan clipboard: could not build/setup integration");
        free(error);
        return false;
    }

    xcb_icccm_set_wm_name(xcon, xwnd, XCB_ATOM_STRING,
                          8, sizeof("arcan-clip")-1, "arcan-clip");

     return true;
}

static void setOwnership(void)
{
    xcb_set_selection_owner_checked(xcon, xwnd, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    xcb_set_selection_owner_checked(xcon, xwnd, atoms.clipboard, XCB_CURRENT_TIME);
}

_Thread_local char* currentMessage = NULL;
static void updateSelection(struct arcan_event* aev)
{
    size_t len = strlen(aev->tgt.message);

/* new, reset */
   if (clip_in.done){
      clip_in.len = 0;
      clip_in.buffer[0] = '\0';
      clip_in.done = false;
   }

/* multipart- append */
   memcpy(&clip_in.buffer[clip_in.len], aev->tgt.message, len);
   clip_in.len += len;
   clip_in.buffer[clip_in.len] = '\0';

   if (!aev->tgt.ioevs[0].iv){
       clip_in.done = true;
       setOwnership();
   }

/* it doesn't seem we have a programatic way of pasting without simulating
 * keypresses, which is in there in between dumb and dangerous - in such a
 * case we should at least check that there are no other keys being held. */
}

static void xfixesSelectionNotify(
    struct proxyWindowData* inWnd, xcb_xfixes_selection_notify_event_t* event)
{
    xcb_get_selection_owner_reply_t *owner =
      xcb_get_selection_owner_reply(
          xcon,
          xcb_get_selection_owner(xcon, atoms.clipboard),
          NULL
      );

/* should also check that it isn't owned by 'the other thread' (if present) */
    if (!owner || owner->owner == 0){
      return;
    }

/* for xfixes we should also send a message so that the arcan end can chose to
 * request and process targets etc. but to also make it easy on ourselves
 * assume we can grab utf8 and forward that immediately - or at least for the
 * primary selection */
    free(owner);
    xcb_convert_selection(xcon, xwnd,
      atoms.clipboard, /* selection */
      atoms.utf8,
      atoms.selection_id,
      event->timestamp
    );
    xcb_flush(xcon);
    trace("xfixesSelectionNotify");
}

static void selectionRequest(
    struct proxyWindowData* inWnd, xcb_selection_request_event_t* event)
{
    if (event->target != XCB_ATOM_STRING &&
        event->target != atoms.utf8 &&
        event->target != atoms.targets){
        trace("selectionRequest(unknown target)");
        return;
    }

/* If we hold the selection, and someone requests our 'targets', then provide
 * the list of what we know we can handle. This will always be UTF8 + any other
 * custom ones that has been provided as messages on the clipboard */
    if (event->target == atoms.targets){
        xcb_atom_t types[] = { atoms.targets, atoms.utf8, XCB_ATOM_STRING };
        trace("requestTargets");
        xcb_change_property_checked(xcon,
                                    XCB_PROP_MODE_REPLACE,
                                    event->requestor,
                                    event->property,
                                    XCB_ATOM_ATOM,
                                    32,
                                    ARRAY_SIZE(types), (unsigned char*) types
                                   );

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
 * other selected target type - the option then is to spin until we are
 * completed or get some other custom behaviour in to let the arcan end know
 * that we really really want something to paste */
    if (!clip_in.done){
        return;
    }

/* this should be chunked similar to how we do it for short messages in arcan,
 * in favour of laziness otoh is that the buffer sizes xcb can deal with are
 * far larger than the message-slot size * message-count, so the longest of
 * multiparts would naturally fit - not the same for streaming transfers though
 * */
    uint32_t len = xcb_get_maximum_request_length(xcon);
    if (clip_in.len > len){
        ErrorF("clipboard item size exceeds limit\n");
        return;
    }

    xcb_change_property_checked(xcon,
                                XCB_PROP_MODE_REPLACE,
                                event->requestor, event->property, event->target,
                                8, clip_in.len, clip_in.buffer
                                );
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
    void* src = xcb_get_property_value(reply);
    size_t len = xcb_get_property_value_length(reply);
    if (!len){
        trace("ignore empty selectionNotify");
        return;
    }

    if (!clip_out.sink){
      trace("noSink - create one");

      if (clip_out.buf){
        free(clip_out.buf);
        clip_out.len = 0;
      }

      clip_out.sink = open_memstream(&clip_out.buf, &clip_out.len);
      if (!clip_out.sink)
          return;
    }

    trace("sendData to sink-File -> %zu", len);

    if (len)
        fwrite(src, len, 1, clip_out.sink);

    if (!len || oneshot){
        fclose(clip_out.sink);
        clip_out.sink = NULL;

/* should send onwards unless it comes from a bchunk, then it has already happened */
        if (clip_out.buf){
            if (logout){
                fprintf(logout, "selection:\n");
                fwrite(clip_out.buf, clip_out.len, 1, logout);
            }
            free(clip_out.buf);
            clip_out.buf = NULL;
        }
    }
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
    trace("send target types");
    for (int i = 0; i < n; i++){
        xcb_atom_t atom = pval[i];
        char *name = atomName(atom);
        trace("target-%s", name);
        snprintf((char*)out.ext.bchunk.extensions,
                 sizeof(out.ext.message), "prop:%s", name);
        arcan_shmif_enqueue(inWnd->cont, &out);
    }

    free(reply);
}

static void propertyNotify(
       struct proxyWindowData *inWnd, xcb_property_notify_event_t * event)
{
    if (event->window == xwnd){
        trace("propertyNotify(selectionWnd)");
        if (event->state == XCB_PROPERTY_NEW_VALUE){
            trace("newValue");
            if (event->atom == atoms.selection_id){
                xcb_get_property_cookie_t cookie;
                cookie = xcb_get_property(xcon,
                                          TRUE, xwnd,
                                          atoms.selection_id,
                                          XCB_GET_PROPERTY_TYPE_ANY,
                                          0,
                                          0x1fffffff);

                xcb_get_property_reply(xcon, cookie, NULL);
                trace("gotSelectionData");
            }
        }
        else {
            trace("unknownProperty");
        }
    }
    else {
        trace("propertyNotify(unknown)");
    }
}

static void selectionNotify(
    struct proxyWindowData* inWnd, xcb_selection_notify_event_t* event)
{
    trace("selectionNotify");

    if (event->selection == atoms.clipboard){
        trace("selectionotify-clipboard");
    } else {
        trace("notify-primary");
    }

/* this is for the exposed set of targets, i.e. which types can the source of
 * the selection handle conversion to. */
    if (event->target == atoms.targets){
        forwardTargets(inWnd, event);
        return;
    }

    xcb_get_property_cookie_t cookie;
    cookie = xcb_get_property(xcon,
                              TRUE, xwnd,
                              atoms.selection_id,
                              atoms.utf8,
/*                              XCB_GET_PROPERTY_TYPE_ANY, */
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
    free(reply);
}

void *arcanClipboardDispatch(struct proxyWindowData* inWnd)
{
    if (!buildWindow(inWnd)){
                arcan_shmif_drop(inWnd->cont);
                free(inWnd);
                return NULL;
        }

    setupAtoms();
    xcb_discard_reply(xcon, xcb_xfixes_query_version(xcon, 1, 0).sequence);

    xcb_prefetch_extension_data(xcon, &xcb_xfixes_id);
    const xcb_query_extension_reply_t *xfixes_id = xcb_get_extension_data(xcon, &xcb_xfixes_id);

/* for the copy mode we both need to query 'what is currently available' and
 * forward that as bchunk-hints, and at the same time just keep copies around
 * for the current selection and forward that as our clipboard output */
    if (!inWnd->paste){
        xcb_xfixes_select_selection_input(
            xcon,
            xwnd,
            atoms.clipboard,
            XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER      |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE
    );
/*
 * this one is quite noisy, since many clients (e.g. chrome) simply update this
 * for each new selection step so we flood the event queues
 * xcb_xfixes_select_selection_input(
            xcon,
            xwnd,
            XCB_ATOM_PRIMARY,
            XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER      |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
            XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE
            );
 */
     }
    else {
        xcb_void_cookie_t cookie =
        xcb_set_selection_owner_checked(xcon,
                                        xwnd,
                                        atoms.clipboard_mgr,
                                        XCB_CURRENT_TIME);

        xcb_generic_error_t *error;
        if ((error = xcb_request_check(xcon, cookie))){
            goto out;
        }

        clip_in.buffer = malloc(4096);
        clip_in.buffer_sz = 4096;
        clip_in.len = 0;
        clip_in.done = false;
        setOwnership();
    }

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

		int pv = 0;
    while (pv >= 0){
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
        while ( (pv = arcan_shmif_poll(inWnd->cont, &aev)) > 0 ){
            if (aev.category != EVENT_TARGET)
                continue;

    /* The model in arcan clipboard is 'push' which is more similar to how a clipboard
     * manager would work, e.g. every time the selection owner changes - copy that and
     * forward. We should have that over bchunk in order to have larger transfer sets. */
            switch (aev.tgt.kind){
                case TARGET_COMMAND_MESSAGE:
                    if (inWnd->paste)
                        updateSelection(&aev);
                break;

                case TARGET_COMMAND_BCHUNK_IN:
                    trace("bchunk.in missing");
                break;

                case TARGET_COMMAND_BCHUNK_OUT:{
                    trace("bchunk.out requested");
                    xcb_atom_t selection = XCB_ATOM_PRIMARY;
                    xcb_atom_t target = atoms.utf8;

                /* only permit one ongoing transfer, this is racey */
                    if (clip_out.sink){
                        fclose(clip_out.sink);
                        if (clip_out.buf){
                            free(clip_out.buf);
                            clip_out.buf = NULL;
                            clip_out.len = 0;
                        }
                    }

                    clip_out.sink =
                        fdopen(arcan_shmif_dupfd(
                                                 aev.tgt.ioevs[0].iv, -1, true),
                               "w");
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

                    xcb_convert_selection(xcon,xwnd, target, selection, atoms.data, XCB_CURRENT_TIME);
                }
                break;
                case TARGET_COMMAND_EXIT:
                    goto out;
                break;
                default:
                break;
            }
        }

        while ((event = xcb_poll_for_event(xcon))){
            switch (event->response_type & ~0x80){
             case XCB_SELECTION_REQUEST:
                 selectionRequest(inWnd, (xcb_selection_request_event_t *) event);
             break;
             case XCB_SELECTION_NOTIFY:
                 selectionNotify(inWnd, (xcb_selection_notify_event_t *) event);
             break;
             case XCB_SELECTION_CLEAR:
                 trace("selectionCleared(lost)");
/* this happens when we have lost the selection to someone else, to get clipboard manager
 * like behavior we should take the selection back and retrieve it from who owns it, but
 * that is problematic when there are multiples .. */
             break;
/* for -redirect with -exec to a non-wm target we kind of want to fill that hole */
						 case XCB_MAP_REQUEST:
                 xcb_map_window(xcon, ((xcb_map_request_event_t*)event)->window);
						 break;
             case XCB_PROPERTY_NOTIFY:
                 propertyNotify(inWnd, (xcb_property_notify_event_t *) event);
             break;
             default:
                trace("unhandled event: %d", (int) event->response_type);
             break;
            }

            if (xfixes_id){
                switch (event->response_type - xfixes_id->first_event){
                case XCB_XFIXES_SELECTION_NOTIFY:
                     xfixesSelectionNotify(inWnd, (xcb_xfixes_selection_notify_event_t *) event);
                break;
                }
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
