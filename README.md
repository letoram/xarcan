Introduction
====
This is a patched Xserver with a KDrive backend that uses the arcan-shmif to
map Xlib/Xcb/X clients to a running arcan instance.

It currently works as a 'desktop in a window', where you supply the window
manager and so on, and use the X side as normal. Some arcan features will be
lost, particularly any customizations done to the keybindings - normal X
controls for defining keyboard maps and overrides are needed.

Compilation
====

        meson build
        cd build ; ninja .

Since most installations tend to have an Xorg already present, and you don't
want to mess with those far too much, the main binary output of interest on
a system with Xorg is simply the Xarcan binary.

Use
====
From a terminal with access to connection primitives (env: ARCAN\_CONNPATH),
just run:

        Xarcan

Then you want to attach a window manager, e.g.

        DISPLAY=:0 i3 &
				DISPLAY=:0 xterm

For multiple instances, you can add :1 (and set DISPLAY=:1 etc. accordingly).

To force the window size and disable resizing behavior, use

There is also the option to forego Xarcan entirely, and use arcan-wayland
from the arcan source repository:

        arcan-wayland -xwl -exec xterm

Planned Changes
====

Here is a list of planned changes for which contributions would be gladly
accepted:

1. Fix GLX-/DRI3- bindings

Some refactoring upstream broke the way we dealt with accelerated handle
passing and accelerated clients. Most of this can probably be translated
from the hw/xwayland parts about gbm/glamor/glx.

2. Swap out Epoxy resolver

Xorg uses epoxy to resolve its GL symbols. For the advanced Arcan parts
with swapping GPUs/resetting clients, we want to (re-)query the symbols
on a device change.

3. Intercept pixmap read calls

Similar to XAce, block attempts at reading screen contents by default,
then swap in any ENCODE- pushed segments to let the arcan instance drive
what screen recording tools etc. see.

4. "Native" mouse cursor

Right now, the mouse cursor is rastered into the buffer. The code should
try to allocate a custom mouse cursor and synchronize that separately.

5. Aclip integration

Request a clipboard via the existing shmif connection, handover-exec it
to aclip with the X display etc. inherited.

6. Keyboard translation

This should also apply to arcan-wayland, but basically a keymap translator
as a pre-step should be viable, see e.g. github.com/39aldo39/klfc.

