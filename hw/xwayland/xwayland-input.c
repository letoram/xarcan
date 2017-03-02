/*
 * Copyright © 2014 Intel Corporation
 * Copyright © 2008 Kristian Høgsberg
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

#include "xwayland.h"

#include <linux/input.h>

#include <sys/mman.h>
#include <xkbsrv.h>
#include <xserver-properties.h>
#include <inpututils.h>
#include <mipointer.h>
#include <mipointrst.h>

struct sync_pending {
    struct xorg_list l;
    DeviceIntPtr pending_dev;
};

static void
xwl_pointer_warp_emulator_handle_motion(struct xwl_pointer_warp_emulator *warp_emulator,
                                        double dx,
                                        double dy,
                                        double dx_unaccel,
                                        double dy_unaccel);
static void
xwl_pointer_warp_emulator_maybe_lock(struct xwl_pointer_warp_emulator *warp_emulator,
                                     struct xwl_window *xwl_window,
                                     SpritePtr sprite,
                                     int x, int y);

static void
xwl_seat_destroy_confined_pointer(struct xwl_seat *xwl_seat);

static void
xwl_pointer_control(DeviceIntPtr device, PtrCtrl *ctrl)
{
    /* Nothing to do, dix handles all settings */
}

static Bool
init_pointer_buttons(DeviceIntPtr device)
{
#define NBUTTONS 10
    BYTE map[NBUTTONS + 1];
    int i = 0;
    Atom btn_labels[NBUTTONS] = { 0 };

    for (i = 1; i <= NBUTTONS; i++)
        map[i] = i;

    btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
    btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
    btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
    btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
    /* don't know about the rest */

    if (!InitButtonClassDeviceStruct(device, NBUTTONS, btn_labels, map))
        return FALSE;

    return TRUE;
}

static int
xwl_pointer_proc(DeviceIntPtr device, int what)
{
#define NAXES 4
    Atom axes_labels[NAXES] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        if (!init_pointer_buttons(device))
            return BadValue;

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y);
        axes_labels[2] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_HWHEEL);
        axes_labels[3] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_WHEEL);

        if (!InitValuatorClassDeviceStruct(device, NAXES, axes_labels,
                                           GetMotionHistorySize(), Absolute))
            return BadValue;

        /* Valuators */
        InitValuatorAxisStruct(device, 0, axes_labels[0],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, 1, axes_labels[1],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, 2, axes_labels[2],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0, Relative);
        InitValuatorAxisStruct(device, 3, axes_labels[3],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 0, 0, 0, Relative);

        SetScrollValuator(device, 2, SCROLL_TYPE_HORIZONTAL, 1.0, SCROLL_FLAG_NONE);
        SetScrollValuator(device, 3, SCROLL_TYPE_VERTICAL, 1.0, SCROLL_FLAG_PREFERRED);

        if (!InitPtrFeedbackClassDeviceStruct(device, xwl_pointer_control))
            return BadValue;

        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;

#undef NBUTTONS
#undef NAXES
}

static int
xwl_pointer_proc_relative(DeviceIntPtr device, int what)
{
#define NAXES 2
    Atom axes_labels[NAXES] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);

        /*
         * We'll never send buttons, but XGetPointerMapping might in certain
         * situations make the client think we have no buttons.
         */
        if (!init_pointer_buttons(device))
            return BadValue;

        if (!InitValuatorClassDeviceStruct(device, NAXES, axes_labels,
                                           GetMotionHistorySize(), Relative))
            return BadValue;

        /* Valuators */
        InitValuatorAxisStruct(device, 0, axes_labels[0],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 1, 0, 1, Relative);
        InitValuatorAxisStruct(device, 1, axes_labels[1],
                               NO_AXIS_LIMITS, NO_AXIS_LIMITS, 1, 0, 1, Relative);

        if (!InitPtrFeedbackClassDeviceStruct(device, xwl_pointer_control))
            return BadValue;

        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;

#undef NAXES
}

static void
xwl_keyboard_control(DeviceIntPtr device, KeybdCtrl *ctrl)
{
}

static int
xwl_keyboard_proc(DeviceIntPtr device, int what)
{
    struct xwl_seat *xwl_seat = device->public.devicePrivate;
    int len;

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;
        if (xwl_seat->keymap)
            len = strnlen(xwl_seat->keymap, xwl_seat->keymap_size);
        else
            len = 0;
        if (!InitKeyboardDeviceStructFromString(device, xwl_seat->keymap,
                                                len,
                                                NULL, xwl_keyboard_control))
            return BadValue;

        return Success;
    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;
}

static int
xwl_touch_proc(DeviceIntPtr device, int what)
{
#define NTOUCHPOINTS 20
#define NBUTTONS 1
#define NAXES 2
    Atom btn_labels[NBUTTONS] = { 0 };
    Atom axes_labels[NAXES] = { 0 };
    BYTE map[NBUTTONS + 1] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_Y);

        if (!InitValuatorClassDeviceStruct(device, NAXES, axes_labels,
                                           GetMotionHistorySize(), Absolute))
            return BadValue;

        if (!InitButtonClassDeviceStruct(device, NBUTTONS, btn_labels, map))
            return BadValue;

        if (!InitTouchClassDeviceStruct(device, NTOUCHPOINTS,
                                        XIDirectTouch, NAXES))
            return BadValue;

        /* Valuators */
        InitValuatorAxisStruct(device, 0, axes_labels[0],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        InitValuatorAxisStruct(device, 1, axes_labels[1],
                               0, 0xFFFF, 10000, 0, 10000, Absolute);
        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    }

    return BadMatch;
#undef NAXES
#undef NBUTTONS
#undef NTOUCHPOINTS
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct xwl_seat *xwl_seat = data;
    DeviceIntPtr dev = xwl_seat->pointer;
    DeviceIntPtr master;
    int i;
    int sx = wl_fixed_to_int(sx_w);
    int sy = wl_fixed_to_int(sy_w);
    int dx, dy;
    ScreenPtr pScreen = xwl_seat->xwl_screen->screen;
    ValuatorMask mask;

    /* There's a race here where if we create and then immediately
     * destroy a surface, we might end up in a state where the Wayland
     * compositor sends us an event for a surface that doesn't exist.
     *
     * Don't process enter events in this case.
     */
    if (surface == NULL)
        return;

    xwl_seat->xwl_screen->serial = serial;
    xwl_seat->pointer_enter_serial = serial;

    xwl_seat->focus_window = wl_surface_get_user_data(surface);
    dx = xwl_seat->focus_window->window->drawable.x;
    dy = xwl_seat->focus_window->window->drawable.y;

    /* We just entered a new xwindow, forget about the old last xwindow */
    xwl_seat->last_xwindow = NullWindow;

    master = GetMaster(dev, POINTER_OR_FLOAT);
    (*pScreen->SetCursorPosition) (dev, pScreen, dx + sx, dy + sy, TRUE);

    miPointerInvalidateSprite(master);

    CheckMotion(NULL, master);

    /* Ideally, X clients shouldn't see these button releases.  When
     * the pointer leaves a window with buttons down, it means that
     * the wayland compositor has grabbed the pointer.  The button
     * release event is consumed by whatever grab in the compositor
     * and won't be sent to clients (the X server is a client).
     * However, we need to reset X's idea of which buttons are up and
     * down, and they're all up (by definition) when the pointer
     * enters a window.  We should figure out a way to swallow these
     * events, perhaps using an X grab whenever the pointer is not in
     * any X window, but for now just send the events. */
    valuator_mask_zero(&mask);
    for (i = 0; i < dev->button->numButtons; i++)
        if (BitIsOn(dev->button->down, i))
            QueuePointerEvents(dev, ButtonRelease, i, 0, &mask);

    /* The last cursor frame we commited before the pointer left one
     * of our surfaces might not have been shown. In that case we'll
     * have a cursor surface frame callback pending which we need to
     * clear so that we can continue submitting new cursor frames. */
    if (xwl_seat->cursor_frame_cb) {
        wl_callback_destroy(xwl_seat->cursor_frame_cb);
        xwl_seat->cursor_frame_cb = NULL;
        xwl_seat_set_cursor(xwl_seat);
    }

    if (xwl_seat->pointer_warp_emulator) {
        xwl_pointer_warp_emulator_maybe_lock(xwl_seat->pointer_warp_emulator,
                                             xwl_seat->focus_window,
                                             NULL, 0, 0);
    }
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface)
{
    struct xwl_seat *xwl_seat = data;
    DeviceIntPtr dev = xwl_seat->pointer;

    xwl_seat->xwl_screen->serial = serial;

    /* The pointer has left a known xwindow, save it for a possible match
     * in sprite_check_lost_focus()
     */
    if (xwl_seat->focus_window) {
        xwl_seat->last_xwindow = xwl_seat->focus_window->window;
        xwl_seat->focus_window = NULL;
        CheckMotion(NULL, GetMaster(dev, POINTER_OR_FLOAT));
    }
}

static void
dispatch_pointer_motion_event(struct xwl_seat *xwl_seat)
{
    ValuatorMask mask;

    if (xwl_seat->pointer_warp_emulator &&
        xwl_seat->pending_pointer_event.has_relative) {
        double dx;
        double dy;
        double dx_unaccel;
        double dy_unaccel;

        dx = xwl_seat->pending_pointer_event.dx;
        dy = xwl_seat->pending_pointer_event.dy;
        dx_unaccel = xwl_seat->pending_pointer_event.dx_unaccel;
        dy_unaccel = xwl_seat->pending_pointer_event.dy_unaccel;
        xwl_pointer_warp_emulator_handle_motion(xwl_seat->pointer_warp_emulator,
                                                dx, dy,
                                                dx_unaccel, dy_unaccel);
    } else if (xwl_seat->pending_pointer_event.has_absolute ||
               xwl_seat->pending_pointer_event.has_relative) {
        int x;
        int y;

        if (xwl_seat->pending_pointer_event.has_absolute) {
            int sx = wl_fixed_to_int(xwl_seat->pending_pointer_event.x);
            int sy = wl_fixed_to_int(xwl_seat->pending_pointer_event.y);
            int dx = xwl_seat->focus_window->window->drawable.x;
            int dy = xwl_seat->focus_window->window->drawable.y;

            x = dx + sx;
            y = dy + sy;
        } else {
            miPointerGetPosition(xwl_seat->pointer, &x, &y);
        }

        valuator_mask_zero(&mask);
        if (xwl_seat->pending_pointer_event.has_relative) {
            double dx_unaccel;
            double dy_unaccel;

            dx_unaccel = xwl_seat->pending_pointer_event.dx_unaccel;
            dy_unaccel = xwl_seat->pending_pointer_event.dy_unaccel;
            valuator_mask_set_absolute_unaccelerated(&mask, 0, x, dx_unaccel);
            valuator_mask_set_absolute_unaccelerated(&mask, 1, y, dy_unaccel);
        } else {
            valuator_mask_set(&mask, 0, x);
            valuator_mask_set(&mask, 1, y);
        }

        QueuePointerEvents(xwl_seat->pointer, MotionNotify, 0,
                           POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
    }

    xwl_seat->pending_pointer_event.has_absolute = FALSE;
    xwl_seat->pending_pointer_event.has_relative = FALSE;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
                      uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct xwl_seat *xwl_seat = data;

    if (!xwl_seat->focus_window)
        return;

    xwl_seat->pending_pointer_event.has_absolute = TRUE;
    xwl_seat->pending_pointer_event.x = sx_w;
    xwl_seat->pending_pointer_event.y = sy_w;

    if (wl_proxy_get_version((struct wl_proxy *) xwl_seat->wl_pointer) < 5)
        dispatch_pointer_motion_event(xwl_seat);
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                      uint32_t time, uint32_t button, uint32_t state)
{
    struct xwl_seat *xwl_seat = data;
    int index;
    ValuatorMask mask;

    xwl_seat->xwl_screen->serial = serial;

    switch (button) {
    case BTN_LEFT:
        index = 1;
        break;
    case BTN_MIDDLE:
        index = 2;
        break;
    case BTN_RIGHT:
        index = 3;
        break;
    default:
        /* Skip indexes 4-7: they are used for vertical and horizontal scroll.
           The rest of the buttons go in order: BTN_SIDE becomes 8, etc. */
        index = 8 + button - BTN_SIDE;
        break;
    }

    valuator_mask_zero(&mask);
    QueuePointerEvents(xwl_seat->pointer,
                       state ? ButtonPress : ButtonRelease, index, 0, &mask);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
                    uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct xwl_seat *xwl_seat = data;
    int index;
    const int divisor = 10;
    ValuatorMask mask;

    switch (axis) {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        index = 3;
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        index = 2;
        break;
    default:
        return;
    }

    valuator_mask_zero(&mask);
    valuator_mask_set_double(&mask, index, wl_fixed_to_double(value) / divisor);
    QueuePointerEvents(xwl_seat->pointer, MotionNotify, 0, POINTER_RELATIVE, &mask);
}

static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
    struct xwl_seat *xwl_seat = data;

    if (!xwl_seat->focus_window)
        return;

    dispatch_pointer_motion_event(xwl_seat);
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer, uint32_t axis_source)
{
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                         uint32_t time, uint32_t axis)
{
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                             uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};

static void
relative_pointer_handle_relative_motion(void *data,
                                        struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1,
                                        uint32_t utime_hi,
                                        uint32_t utime_lo,
                                        wl_fixed_t dxf,
                                        wl_fixed_t dyf,
                                        wl_fixed_t dx_unaccelf,
                                        wl_fixed_t dy_unaccelf)
{
    struct xwl_seat *xwl_seat = data;

    xwl_seat->pending_pointer_event.has_relative = TRUE;
    xwl_seat->pending_pointer_event.dx = wl_fixed_to_double(dxf);
    xwl_seat->pending_pointer_event.dy = wl_fixed_to_double(dyf);
    xwl_seat->pending_pointer_event.dx_unaccel = wl_fixed_to_double(dx_unaccelf);
    xwl_seat->pending_pointer_event.dy_unaccel = wl_fixed_to_double(dy_unaccelf);

    if (!xwl_seat->focus_window)
        return;

    if (wl_proxy_get_version((struct wl_proxy *) xwl_seat->wl_pointer) < 5)
        dispatch_pointer_motion_event(xwl_seat);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    relative_pointer_handle_relative_motion,
};

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    struct xwl_seat *xwl_seat = data;
    uint32_t *k, *end;

    xwl_seat->xwl_screen->serial = serial;

    end = (uint32_t *) ((char *) xwl_seat->keys.data + xwl_seat->keys.size);
    for (k = xwl_seat->keys.data; k < end; k++) {
        if (*k == key)
            *k = *--end;
    }
    xwl_seat->keys.size = (char *) end - (char *) xwl_seat->keys.data;
    if (state) {
        k = wl_array_add(&xwl_seat->keys, sizeof *k);
        *k = key;
    }

    QueueKeyboardEvents(xwl_seat->keyboard,
                        state ? KeyPress : KeyRelease, key + 8);
}

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                       uint32_t format, int fd, uint32_t size)
{
    struct xwl_seat *xwl_seat = data;
    DeviceIntPtr master;
    XkbDescPtr xkb;
    XkbChangesRec changes = { 0 };

    if (xwl_seat->keymap)
        munmap(xwl_seat->keymap, xwl_seat->keymap_size);

    xwl_seat->keymap_size = size;
    xwl_seat->keymap = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (xwl_seat->keymap == MAP_FAILED) {
        xwl_seat->keymap_size = 0;
        xwl_seat->keymap = NULL;
        goto out;
    }

    xkb = XkbCompileKeymapFromString(xwl_seat->keyboard, xwl_seat->keymap,
                                     strnlen(xwl_seat->keymap,
                                             xwl_seat->keymap_size));
    if (!xkb)
        goto out;

    XkbUpdateDescActions(xkb, xkb->min_key_code, XkbNumKeys(xkb), &changes);

    if (xwl_seat->keyboard->key)
        /* Keep the current controls */
        XkbCopyControls(xkb, xwl_seat->keyboard->key->xkbInfo->desc);

    XkbDeviceApplyKeymap(xwl_seat->keyboard, xkb);

    master = GetMaster(xwl_seat->keyboard, MASTER_KEYBOARD);
    if (master && master->lastSlave == xwl_seat->keyboard)
        XkbDeviceApplyKeymap(master, xkb);

    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);

 out:
    close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                      uint32_t serial,
                      struct wl_surface *surface, struct wl_array *keys)
{
    struct xwl_seat *xwl_seat = data;
    uint32_t *k;

    xwl_seat->xwl_screen->serial = serial;
    xwl_seat->keyboard_focus = surface;

    wl_array_copy(&xwl_seat->keys, keys);
    wl_array_for_each(k, &xwl_seat->keys)
        QueueKeyboardEvents(xwl_seat->keyboard, EnterNotify, *k + 8);
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                      uint32_t serial, struct wl_surface *surface)
{
    struct xwl_seat *xwl_seat = data;
    uint32_t *k;

    xwl_seat->xwl_screen->serial = serial;

    wl_array_for_each(k, &xwl_seat->keys)
        QueueKeyboardEvents(xwl_seat->keyboard, LeaveNotify, *k + 8);

    xwl_seat->keyboard_focus = NULL;
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, uint32_t mods_depressed,
                          uint32_t mods_latched, uint32_t mods_locked,
                          uint32_t group)
{
    struct xwl_seat *xwl_seat = data;
    DeviceIntPtr dev;
    XkbStateRec old_state, *new_state;
    xkbStateNotify sn;
    CARD16 changed;

    mieqProcessInputEvents();

    for (dev = inputInfo.devices; dev; dev = dev->next) {
        if (dev != xwl_seat->keyboard &&
            dev != GetMaster(xwl_seat->keyboard, MASTER_KEYBOARD))
            continue;

        old_state = dev->key->xkbInfo->state;
        new_state = &dev->key->xkbInfo->state;

        new_state->locked_group = group & XkbAllGroupsMask;
        new_state->base_mods = mods_depressed & XkbAllModifiersMask;
        new_state->locked_mods = mods_locked & XkbAllModifiersMask;
        XkbLatchModifiers(dev, XkbAllModifiersMask,
                          mods_latched & XkbAllModifiersMask);

        XkbComputeDerivedState(dev->key->xkbInfo);

        changed = XkbStateChangedFlags(&old_state, new_state);
        if (!changed)
            continue;

        sn.keycode = 0;
        sn.eventType = 0;
        sn.requestMajor = XkbReqCode;
        sn.requestMinor = X_kbLatchLockState;   /* close enough */
        sn.changed = changed;
        XkbSendStateNotify(dev, &sn);
    }
}

static void
remove_sync_pending(DeviceIntPtr dev)
{
    struct xwl_seat *xwl_seat = dev->public.devicePrivate;
    struct sync_pending *p, *npd;

    xorg_list_for_each_entry_safe(p, npd, &xwl_seat->sync_pending, l) {
        if (p->pending_dev == dev) {
            xorg_list_del(&xwl_seat->sync_pending);
            free (p);
            return;
        }
    }
}

static void
sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    DeviceIntPtr dev = (DeviceIntPtr) data;

    remove_sync_pending(dev);
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
   sync_callback
};

static Bool
keyboard_check_repeat (DeviceIntPtr dev, XkbSrvInfoPtr xkbi, unsigned key)
{
    struct xwl_seat *xwl_seat = dev->public.devicePrivate;
    struct xwl_screen *xwl_screen = xwl_seat->xwl_screen;
    struct wl_callback *callback;
    struct sync_pending *p;

    /* Make sure we didn't miss a possible reply from the compositor */
    xwl_sync_events (xwl_screen);

    xorg_list_for_each_entry(p, &xwl_seat->sync_pending, l) {
        if (p->pending_dev == dev) {
            ErrorF("Key repeat discarded, Wayland compositor doesn't "
                   "seem to be processing events fast enough!\n");

            return FALSE;
        }
    }

    p = xnfalloc(sizeof(struct sync_pending));
    p->pending_dev = dev;
    callback = wl_display_sync (xwl_screen->display);
    xorg_list_add(&p->l, &xwl_seat->sync_pending);

    wl_callback_add_listener(callback, &sync_listener, dev);

    return TRUE;
}

static void
keyboard_handle_repeat_info (void *data, struct wl_keyboard *keyboard,
                             int32_t rate, int32_t delay)
{
    struct xwl_seat *xwl_seat = data;
    DeviceIntPtr dev;
    XkbControlsPtr ctrl;

    if (rate < 0 || delay < 0) {
        ErrorF("Wrong rate/delay: %d, %d\n", rate, delay);
        return;
    }

    for (dev = inputInfo.devices; dev; dev = dev->next) {
        if (dev != xwl_seat->keyboard &&
            dev != GetMaster(xwl_seat->keyboard, MASTER_KEYBOARD))
            continue;

        if (rate != 0) {
            ctrl = dev->key->xkbInfo->desc->ctrls;
            ctrl->repeat_delay = delay;
            /* rate is number of keys per second */
            ctrl->repeat_interval = 1000 / rate;

            XkbSetRepeatKeys(dev, -1, AutoRepeatModeOn);
        } else
            XkbSetRepeatKeys(dev, -1, AutoRepeatModeOff);
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static struct xwl_touch *
xwl_seat_lookup_touch(struct xwl_seat *xwl_seat, int32_t id)
{
    struct xwl_touch *xwl_touch, *next_xwl_touch;

    xorg_list_for_each_entry_safe(xwl_touch, next_xwl_touch,
                                  &xwl_seat->touches, link_touch) {
        if (xwl_touch->id == id)
            return xwl_touch;
    }

    return NULL;
}

static void
xwl_touch_send_event(struct xwl_touch *xwl_touch,
                     struct xwl_seat *xwl_seat, int type)
{
    double dx, dy, x, y;
    ValuatorMask mask;

    dx = xwl_touch->window->window->drawable.x;
    dy = xwl_touch->window->window->drawable.y;

    x = (dx + xwl_touch->x) * 0xFFFF / xwl_seat->xwl_screen->width;
    y = (dy + xwl_touch->y) * 0xFFFF / xwl_seat->xwl_screen->height;

    valuator_mask_zero(&mask);
    valuator_mask_set_double(&mask, 0, x);
    valuator_mask_set_double(&mask, 1, y);
    QueueTouchEvents(xwl_seat->touch, type, xwl_touch->id, 0, &mask);
}

static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
                  uint32_t serial, uint32_t time,
                  struct wl_surface *surface,
                  int32_t id, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct xwl_seat *xwl_seat = data;
    struct xwl_touch *xwl_touch;

    if (surface == NULL)
        return;

    xwl_touch = calloc(1, sizeof *xwl_touch);
    if (xwl_touch == NULL) {
        ErrorF("%s: ENOMEM\n", __func__);
        return;
    }

    xwl_touch->window = wl_surface_get_user_data(surface);
    xwl_touch->id = id;
    xwl_touch->x = wl_fixed_to_int(sx_w);
    xwl_touch->y = wl_fixed_to_int(sy_w);
    xorg_list_add(&xwl_touch->link_touch, &xwl_seat->touches);

    xwl_touch_send_event(xwl_touch, xwl_seat, XI_TouchBegin);
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
                uint32_t serial, uint32_t time, int32_t id)
{
    struct xwl_touch *xwl_touch;
    struct xwl_seat *xwl_seat = data;

    xwl_touch = xwl_seat_lookup_touch(xwl_seat, id);

    if (!xwl_touch)
        return;

    xwl_touch_send_event(xwl_touch, xwl_seat, XI_TouchEnd);
    xorg_list_del(&xwl_touch->link_touch);
    free(xwl_touch);
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
                    uint32_t time, int32_t id,
                    wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct xwl_seat *xwl_seat = data;
    struct xwl_touch *xwl_touch;

    xwl_touch = xwl_seat_lookup_touch(xwl_seat, id);

    if (!xwl_touch)
        return;

    xwl_touch->x = wl_fixed_to_int(sx_w);
    xwl_touch->y = wl_fixed_to_int(sy_w);
    xwl_touch_send_event(xwl_touch, xwl_seat, XI_TouchUpdate);
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
    struct xwl_seat *xwl_seat = data;
    struct xwl_touch *xwl_touch, *next_xwl_touch;

    xorg_list_for_each_entry_safe(xwl_touch, next_xwl_touch,
                                  &xwl_seat->touches, link_touch) {
        /* We can't properly notify of cancellation to the X client
         * once it thinks it has the ownership, send at least a
         * TouchEnd event.
         */
        xwl_touch_send_event(xwl_touch, xwl_seat, XI_TouchEnd);
        xorg_list_del(&xwl_touch->link_touch);
        free(xwl_touch);
    }
}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel
};

static DeviceIntPtr
add_device(struct xwl_seat *xwl_seat,
           const char *driver, DeviceProc device_proc)
{
    DeviceIntPtr dev = NULL;
    static Atom type_atom;
    char name[32];

    dev = AddInputDevice(serverClient, device_proc, TRUE);
    if (dev == NULL)
        return NULL;

    if (type_atom == None)
        type_atom = MakeAtom(driver, strlen(driver), TRUE);
    snprintf(name, sizeof name, "%s:%d", driver, xwl_seat->id);
    AssignTypeAndName(dev, type_atom, name);
    dev->public.devicePrivate = xwl_seat;
    dev->type = SLAVE;
    dev->spriteInfo->spriteOwner = FALSE;

    return dev;
}

static void
init_pointer(struct xwl_seat *xwl_seat)
{
    xwl_seat->wl_pointer = wl_seat_get_pointer(xwl_seat->seat);
    wl_pointer_add_listener(xwl_seat->wl_pointer,
                            &pointer_listener, xwl_seat);

    if (xwl_seat->pointer == NULL) {
        xwl_seat_set_cursor(xwl_seat);
        xwl_seat->pointer =
            add_device(xwl_seat, "xwayland-pointer", xwl_pointer_proc);
        ActivateDevice(xwl_seat->pointer, TRUE);
    }
    EnableDevice(xwl_seat->pointer, TRUE);
}

static void
release_pointer(struct xwl_seat *xwl_seat)
{
    wl_pointer_release(xwl_seat->wl_pointer);
    xwl_seat->wl_pointer = NULL;

    if (xwl_seat->pointer)
        DisableDevice(xwl_seat->pointer, TRUE);
}

static void
init_relative_pointer(struct xwl_seat *xwl_seat)
{
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager =
        xwl_seat->xwl_screen->relative_pointer_manager;

    if (relative_pointer_manager) {
        xwl_seat->wp_relative_pointer =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                relative_pointer_manager, xwl_seat->wl_pointer);
        zwp_relative_pointer_v1_add_listener(xwl_seat->wp_relative_pointer,
                                             &relative_pointer_listener,
                                             xwl_seat);
    }

    if (xwl_seat->relative_pointer == NULL) {
        xwl_seat->relative_pointer =
            add_device(xwl_seat, "xwayland-relative-pointer",
                       xwl_pointer_proc_relative);
        ActivateDevice(xwl_seat->relative_pointer, TRUE);
    }
    EnableDevice(xwl_seat->relative_pointer, TRUE);
}

static void
release_relative_pointer(struct xwl_seat *xwl_seat)
{
    if (xwl_seat->wp_relative_pointer) {
        zwp_relative_pointer_v1_destroy(xwl_seat->wp_relative_pointer);
        xwl_seat->wp_relative_pointer = NULL;
    }

    if (xwl_seat->relative_pointer)
        DisableDevice(xwl_seat->relative_pointer, TRUE);
}

static void
init_keyboard(struct xwl_seat *xwl_seat)
{
    xwl_seat->wl_keyboard = wl_seat_get_keyboard(xwl_seat->seat);
    wl_keyboard_add_listener(xwl_seat->wl_keyboard,
                             &keyboard_listener, xwl_seat);

    if (xwl_seat->keyboard == NULL) {
        xwl_seat->keyboard =
            add_device(xwl_seat, "xwayland-keyboard", xwl_keyboard_proc);
        ActivateDevice(xwl_seat->keyboard, TRUE);
    }
    EnableDevice(xwl_seat->keyboard, TRUE);
    xwl_seat->keyboard->key->xkbInfo->checkRepeat = keyboard_check_repeat;
}

static void
release_keyboard(struct xwl_seat *xwl_seat)
{
    wl_keyboard_release(xwl_seat->wl_keyboard);
    xwl_seat->wl_keyboard = NULL;

    if (xwl_seat->keyboard) {
        remove_sync_pending(xwl_seat->keyboard);
        DisableDevice(xwl_seat->keyboard, TRUE);
    }
}

static void
init_touch(struct xwl_seat *xwl_seat)
{
    xwl_seat->wl_touch = wl_seat_get_touch(xwl_seat->seat);
    wl_touch_add_listener(xwl_seat->wl_touch,
                          &touch_listener, xwl_seat);

    if (xwl_seat->touch == NULL) {
        xwl_seat->touch =
            add_device(xwl_seat, "xwayland-touch", xwl_touch_proc);
        ActivateDevice(xwl_seat->touch, TRUE);
    }
    EnableDevice(xwl_seat->touch, TRUE);

}

static void
release_touch(struct xwl_seat *xwl_seat)
{
    wl_touch_release(xwl_seat->wl_touch);
    xwl_seat->wl_touch = NULL;

    if (xwl_seat->touch)
        DisableDevice(xwl_seat->touch, TRUE);
}

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
                         enum wl_seat_capability caps)
{
    struct xwl_seat *xwl_seat = data;

    if (caps & WL_SEAT_CAPABILITY_POINTER && xwl_seat->wl_pointer == NULL) {
        init_pointer(xwl_seat);
        init_relative_pointer(xwl_seat);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && xwl_seat->wl_pointer) {
        release_pointer(xwl_seat);
        release_relative_pointer(xwl_seat);
    }

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD && xwl_seat->wl_keyboard == NULL) {
        init_keyboard(xwl_seat);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && xwl_seat->wl_keyboard) {
        release_keyboard(xwl_seat);
    }

    if (caps & WL_SEAT_CAPABILITY_TOUCH && xwl_seat->wl_touch == NULL) {
        init_touch(xwl_seat);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && xwl_seat->wl_touch) {
        release_touch(xwl_seat);
    }

    xwl_seat->xwl_screen->expecting_event--;
}

static void
seat_handle_name(void *data, struct wl_seat *seat,
                 const char *name)
{

}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name
};

static void
create_input_device(struct xwl_screen *xwl_screen, uint32_t id, uint32_t version)
{
    struct xwl_seat *xwl_seat;

    xwl_seat = calloc(1, sizeof *xwl_seat);
    if (xwl_seat == NULL) {
        ErrorF("%s: ENOMEM\n", __func__);
        return;
    }

    xwl_seat->xwl_screen = xwl_screen;
    xorg_list_add(&xwl_seat->link, &xwl_screen->seat_list);

    xwl_seat->seat =
        wl_registry_bind(xwl_screen->registry, id,
                         &wl_seat_interface, min(version, 5));
    xwl_seat->id = id;

    xwl_seat->cursor = wl_compositor_create_surface(xwl_screen->compositor);
    wl_seat_add_listener(xwl_seat->seat, &seat_listener, xwl_seat);
    wl_array_init(&xwl_seat->keys);

    xorg_list_init(&xwl_seat->touches);
    xorg_list_init(&xwl_seat->sync_pending);
}

void
xwl_seat_destroy(struct xwl_seat *xwl_seat)
{
    struct xwl_touch *xwl_touch, *next_xwl_touch;
    struct sync_pending *p, *npd;

    xorg_list_for_each_entry_safe(xwl_touch, next_xwl_touch,
                                  &xwl_seat->touches, link_touch) {
        xorg_list_del(&xwl_touch->link_touch);
        free(xwl_touch);
    }

    xorg_list_for_each_entry_safe(p, npd, &xwl_seat->sync_pending, l) {
        xorg_list_del(&xwl_seat->sync_pending);
        free (p);
    }

    wl_seat_destroy(xwl_seat->seat);
    wl_surface_destroy(xwl_seat->cursor);
    if (xwl_seat->cursor_frame_cb)
        wl_callback_destroy(xwl_seat->cursor_frame_cb);
    wl_array_release(&xwl_seat->keys);
    free(xwl_seat);
}

static void
init_relative_pointer_manager(struct xwl_screen *xwl_screen,
                              uint32_t id, uint32_t version)
{
    xwl_screen->relative_pointer_manager =
        wl_registry_bind(xwl_screen->registry, id,
                         &zwp_relative_pointer_manager_v1_interface,
                         1);
}

static void
init_pointer_constraints(struct xwl_screen *xwl_screen,
                         uint32_t id, uint32_t version)
{
    xwl_screen->pointer_constraints =
        wl_registry_bind(xwl_screen->registry, id,
                         &zwp_pointer_constraints_v1_interface,
                         1);
}

static void
input_handler(void *data, struct wl_registry *registry, uint32_t id,
              const char *interface, uint32_t version)
{
    struct xwl_screen *xwl_screen = data;

    if (strcmp(interface, "wl_seat") == 0 && version >= 3) {
        create_input_device(xwl_screen, id, version);
        xwl_screen->expecting_event++;
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        init_relative_pointer_manager(xwl_screen, id, version);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        init_pointer_constraints(xwl_screen, id, version);
    }
}

static void
global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener input_listener = {
    input_handler,
    global_remove,
};

Bool
LegalModifier(unsigned int key, DeviceIntPtr pDev)
{
    return TRUE;
}

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

static Bool
sprite_check_lost_focus(SpritePtr sprite, WindowPtr window)
{
    DeviceIntPtr device, master;
    struct xwl_seat *xwl_seat;

    for (device = inputInfo.devices; device; device = device->next) {
        /* Ignore non-wayland devices */
        if (device->deviceProc == xwl_pointer_proc &&
            device->spriteInfo->sprite == sprite)
            break;
    }

    if (!device)
        return FALSE;

    xwl_seat = device->public.devicePrivate;

    master = GetMaster(device, POINTER_OR_FLOAT);
    if (!master || !master->lastSlave)
        return FALSE;

    /* We do want the last active slave, we only check on slave xwayland
     * devices so we can find out the xwl_seat, but those don't actually own
     * their sprite, so the match doesn't mean a lot.
     */
    if (master->lastSlave == xwl_seat->pointer &&
        xwl_seat->focus_window == NULL &&
        xwl_seat->last_xwindow != NullWindow &&
        IsParent(xwl_seat->last_xwindow, window))
        return TRUE;

    return FALSE;
}

static WindowPtr
xwl_xy_to_window(ScreenPtr screen, SpritePtr sprite, int x, int y)
{
    struct xwl_screen *xwl_screen;
    WindowPtr ret;

    xwl_screen = xwl_screen_get(screen);

    screen->XYToWindow = xwl_screen->XYToWindow;
    ret = screen->XYToWindow(screen, sprite, x, y);
    xwl_screen->XYToWindow = screen->XYToWindow;
    screen->XYToWindow = xwl_xy_to_window;

    /* If the device controlling the sprite has left the Wayland surface but
     * the DIX still finds the pointer within the X11 window, it means that
     * the pointer has crossed to another native Wayland window, in this
     * case, pretend we entered the root window so that a LeaveNotify
     * event is emitted.
     */
    if (sprite_check_lost_focus(sprite, ret)) {
        sprite->spriteTraceGood = 1;
        return sprite->spriteTrace[0];
    }

    return ret;
}

void
xwl_seat_clear_touch(struct xwl_seat *xwl_seat, WindowPtr window)
{
    struct xwl_touch *xwl_touch, *next_xwl_touch;

    xorg_list_for_each_entry_safe(xwl_touch, next_xwl_touch,
                                  &xwl_seat->touches, link_touch) {
        if (xwl_touch->window->window == window) {
            xorg_list_del(&xwl_touch->link_touch);
            free(xwl_touch);
        }
    }
}

static void
xwl_pointer_warp_emulator_set_fake_pos(struct xwl_pointer_warp_emulator *warp_emulator,
                                       int x,
                                       int y)
{
    struct zwp_locked_pointer_v1 *locked_pointer =
        warp_emulator->locked_pointer;
    WindowPtr window;
    int sx, sy;

    if (!warp_emulator->locked_pointer)
        return;

    if (!warp_emulator->xwl_seat->focus_window)
        return;

    window = warp_emulator->xwl_seat->focus_window->window;
    if (x >= window->drawable.x ||
        y >= window->drawable.y ||
        x < (window->drawable.x + window->drawable.width) ||
        y < (window->drawable.y + window->drawable.height)) {
        sx = x - window->drawable.x;
        sy = y - window->drawable.y;
        zwp_locked_pointer_v1_set_cursor_position_hint(locked_pointer,
                                                       wl_fixed_from_int(sx),
                                                       wl_fixed_from_int(sy));
        wl_surface_commit(warp_emulator->xwl_seat->focus_window->surface);
    }
}

static Bool
xwl_pointer_warp_emulator_is_locked(struct xwl_pointer_warp_emulator *warp_emulator)
{
    if (warp_emulator->locked_pointer)
        return TRUE;
    else
        return FALSE;
}

static void
xwl_pointer_warp_emulator_lock(struct xwl_pointer_warp_emulator *warp_emulator)
{
    struct xwl_seat *xwl_seat = warp_emulator->xwl_seat;
    struct xwl_screen *xwl_screen = xwl_seat->xwl_screen;
    struct zwp_pointer_constraints_v1 *pointer_constraints =
        xwl_screen->pointer_constraints;
    struct xwl_window *lock_window = xwl_seat->focus_window;

    warp_emulator->locked_window = lock_window;

    warp_emulator->locked_pointer =
        zwp_pointer_constraints_v1_lock_pointer(pointer_constraints,
                                                lock_window->surface,
                                                xwl_seat->wl_pointer,
                                                NULL,
                                                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
}

static void
xwl_pointer_warp_emulator_maybe_lock(struct xwl_pointer_warp_emulator *warp_emulator,
                                     struct xwl_window *xwl_window,
                                     SpritePtr sprite,
                                     int x, int y)
{
    struct xwl_seat *xwl_seat = warp_emulator->xwl_seat;
    GrabPtr pointer_grab = xwl_seat->pointer->deviceGrab.grab;

    if (warp_emulator->locked_pointer)
        return;

    /*
     * If there is no grab, and the window doesn't have pointer focus, ignore
     * the warp, as under Wayland it won't receive input anyway.
     */
    if (!pointer_grab && xwl_seat->focus_window != xwl_window)
        return;

    /*
     * If there is a grab, but it's not an ownerEvents grab and the destination
     * is not the pointer focus, ignore it, as events wouldn't be delivered
     * there anyway.
     */
    if (pointer_grab &&
        !pointer_grab->ownerEvents &&
        XYToWindow(sprite, x, y) != xwl_seat->focus_window->window)
        return;

    xwl_pointer_warp_emulator_lock(warp_emulator);
}

static void
xwl_pointer_warp_emulator_warp(struct xwl_pointer_warp_emulator *warp_emulator,
                               struct xwl_window *xwl_window,
                               SpritePtr sprite,
                               int x, int y)
{
    xwl_pointer_warp_emulator_maybe_lock(warp_emulator,
                                         xwl_window,
                                         sprite,
                                         x, y);
    xwl_pointer_warp_emulator_set_fake_pos(warp_emulator, x, y);
}

static void
xwl_pointer_warp_emulator_handle_motion(struct xwl_pointer_warp_emulator *warp_emulator,
                                        double dx,
                                        double dy,
                                        double dx_unaccel,
                                        double dy_unaccel)
{
    struct xwl_seat *xwl_seat = warp_emulator->xwl_seat;
    ValuatorMask mask;
    WindowPtr window;
    int x, y;

    valuator_mask_zero(&mask);
    valuator_mask_set_unaccelerated(&mask, 0, dx, dx_unaccel);
    valuator_mask_set_unaccelerated(&mask, 1, dy, dy_unaccel);

    QueuePointerEvents(xwl_seat->relative_pointer, MotionNotify, 0,
                       POINTER_RELATIVE, &mask);

    window = xwl_seat->focus_window->window;
    miPointerGetPosition(xwl_seat->pointer, &x, &y);

    if (xwl_pointer_warp_emulator_is_locked(warp_emulator) &&
        xwl_seat->cursor_confinement_window != warp_emulator->locked_window &&
        (x < window->drawable.x ||
         y < window->drawable.y ||
         x >= (window->drawable.x + window->drawable.width) ||
         y >= (window->drawable.y + window->drawable.height)))
        xwl_seat_destroy_pointer_warp_emulator(xwl_seat);
    else
        xwl_pointer_warp_emulator_set_fake_pos(warp_emulator, x, y);
}

static struct xwl_pointer_warp_emulator *
xwl_pointer_warp_emulator_create(struct xwl_seat *xwl_seat)
{
    struct xwl_pointer_warp_emulator *warp_emulator;

    warp_emulator = calloc(1, sizeof *warp_emulator);
    if (!warp_emulator) {
        ErrorF("%s: ENOMEM\n", __func__);
        return NULL;
    }

    warp_emulator->xwl_seat = xwl_seat;

    return warp_emulator;
}

static void
xwl_pointer_warp_emulator_destroy(struct xwl_pointer_warp_emulator *warp_emulator)
{
    if (warp_emulator->locked_pointer)
        zwp_locked_pointer_v1_destroy(warp_emulator->locked_pointer);
    free(warp_emulator);
}

static void
xwl_seat_create_pointer_warp_emulator(struct xwl_seat *xwl_seat)
{
    if (xwl_seat->confined_pointer)
        xwl_seat_destroy_confined_pointer(xwl_seat);

    xwl_seat->pointer_warp_emulator =
        xwl_pointer_warp_emulator_create(xwl_seat);
}

static Bool
xwl_seat_can_emulate_pointer_warp(struct xwl_seat *xwl_seat)
{
    struct xwl_screen *xwl_screen = xwl_seat->xwl_screen;

    if (!xwl_screen->relative_pointer_manager)
        return FALSE;

    if (!xwl_screen->pointer_constraints)
        return FALSE;

    return TRUE;
}

void
xwl_seat_emulate_pointer_warp(struct xwl_seat *xwl_seat,
                              struct xwl_window *xwl_window,
                              SpritePtr sprite,
                              int x, int y)
{
    if (!xwl_seat_can_emulate_pointer_warp(xwl_seat))
        return;

    if (xwl_seat->x_cursor != NULL)
        return;

    if (!xwl_seat->pointer_warp_emulator)
        xwl_seat_create_pointer_warp_emulator(xwl_seat);

    if (!xwl_seat->pointer_warp_emulator)
        return;

    xwl_pointer_warp_emulator_warp(xwl_seat->pointer_warp_emulator,
                                   xwl_window,
                                   sprite,
                                   x, y);
}

void
xwl_seat_cursor_visibility_changed(struct xwl_seat *xwl_seat)
{
    if (xwl_seat->pointer_warp_emulator && xwl_seat->x_cursor != NULL)
        xwl_seat_destroy_pointer_warp_emulator(xwl_seat);
}

void
xwl_seat_destroy_pointer_warp_emulator(struct xwl_seat *xwl_seat)
{
    if (!xwl_seat->pointer_warp_emulator)
        return;

    xwl_pointer_warp_emulator_destroy(xwl_seat->pointer_warp_emulator);
    xwl_seat->pointer_warp_emulator = NULL;

    if (xwl_seat->cursor_confinement_window) {
        xwl_seat_confine_pointer(xwl_seat,
                                 xwl_seat->cursor_confinement_window);
    }
}

void
xwl_seat_confine_pointer(struct xwl_seat *xwl_seat,
                         struct xwl_window *xwl_window)
{
    struct zwp_pointer_constraints_v1 *pointer_constraints =
        xwl_seat->xwl_screen->pointer_constraints;

    if (!pointer_constraints)
        return;

    if (xwl_seat->cursor_confinement_window == xwl_window &&
        xwl_seat->confined_pointer)
        return;

    xwl_seat_unconfine_pointer(xwl_seat);

    xwl_seat->cursor_confinement_window = xwl_window;

    if (xwl_seat->pointer_warp_emulator)
        return;

    xwl_seat->confined_pointer =
        zwp_pointer_constraints_v1_confine_pointer(pointer_constraints,
                                                   xwl_window->surface,
                                                   xwl_seat->wl_pointer,
                                                   NULL,
                                                   ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
}

static void
xwl_seat_destroy_confined_pointer(struct xwl_seat *xwl_seat)
{
    zwp_confined_pointer_v1_destroy(xwl_seat->confined_pointer);
    xwl_seat->confined_pointer = NULL;
}

void
xwl_seat_unconfine_pointer(struct xwl_seat *xwl_seat)
{
    xwl_seat->cursor_confinement_window = NULL;

    if (xwl_seat->confined_pointer)
        xwl_seat_destroy_confined_pointer(xwl_seat);
}

void
InitInput(int argc, char *argv[])
{
    ScreenPtr pScreen = screenInfo.screens[0];
    struct xwl_screen *xwl_screen = xwl_screen_get(pScreen);

    mieqInit();

    xwl_screen->input_registry = wl_display_get_registry(xwl_screen->display);
    wl_registry_add_listener(xwl_screen->input_registry, &input_listener,
                             xwl_screen);

    xwl_screen->XYToWindow = pScreen->XYToWindow;
    pScreen->XYToWindow = xwl_xy_to_window;

    wl_display_roundtrip(xwl_screen->display);
    while (xwl_screen->expecting_event)
        wl_display_roundtrip(xwl_screen->display);
}

void
CloseInput(void)
{
    mieqFini();
}
