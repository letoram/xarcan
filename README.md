Introduction
====
This is a patched Xserver with a KDrive backend that uses the arcan-shmif to
map Xlib/Xcb/X clients to a running arcan instance.

The initial approach was simply to let the waybridge tool handle X as well,
but after some experimentation, the nested translation model proved quite
painful to work with, warranting something easier that can be used in the
cases where one would not want the overhead of running a full linux-dist
inside a QEMU session (the preferred compatibility approach).

Compilation
====
Normal autohell style compilation. Relevant compilation flags are:
    --enable-kdrive --enable-xarcan --disable-xorg --disable-xwayland
		--disable-xfvb --enable-glamor --enable-glx --no-create --no-recursion

Though a ton of other X features (e.g. fonts etc.) could also be disabled,
if you find a better set of compilation options for a feature- complete yet
tiny X server, send patches to this document :)

Use
====
From a terminal with access to connection primitives (env: ARCAN\_CONNPATH),
just run hw/kdrive/arcan/Xarcan to start bridging connections.

Limitations
====
The strategy used here is to contain all X clients within one logical window,
for mapping single- client windows to corresponding Arcan primitives, the plan
is still to go through Wayland/XWayland, though that is still somewhat
uncertain.

One big limitation is that the keyboard mapping/remapping features that some
arcan appls like durden employ will not work here. The server only uses the
raw OS keycodes mapped into the sanity absorbing black chasm that is Xkb. Use
the normal Xserver arguments to pick whatever layout you need.

TODO
====
There are still a number of TODOs before all X clients can be successfully
bridged, here are the current big points:

- [ ] Damage- Regions
- [ ] Framebuffer-GL handle mapping
- [ ] Randr/DISPLAYHINT resizing
- [ ] Synchronization
        - [ ] Accelerated Cursor
        - [ ] Glamor/dri3
    - [ ] Normal drawing
    - [ ] Xv
    - [ ] GLX
    - [ ] Clipboard Integration
        - [ ] Pasteboard
        - [ ] Text
        - [ ] Bchunk
        - [ ] Video
