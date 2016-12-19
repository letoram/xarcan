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
    --disable-xvfb --enable-glamor --enable-glx

pkg\_info need to find the normal arcan-shmif, arcan-shmifext libraries.

Though a ton of other X features, could/should also be disabled.
If you find a better set of compilation options for a feature- complete yet
tiny X server, let me know.

Use
====
From a terminal with access to connection primitives (env: ARCAN\_CONNPATH),
just run hw/kdrive/arcan/Xarcan to start bridging connections.

For multiple instances, you can add :1 (and set DISPLAY=:1 etc. accordingly)
and so one, forcing size with -screen 800x600 -no-dynamic and enable glamor
(GL rendering with handle passing( using -glamor (see issues). The title and
identity can be scriptably controlled with -aident identstr -atitle titlestr

Limitations
====
The strategy used here is to contain all X clients within one logical window,
for mapping single- client windows to corresponding Arcan primitives, the plan
is still to go through Wayland/XWayland, though that is still somewhat
uncertain. The exception is if a DRI3 client goes full-screen, then the buffer
we passed should switch to that one immediately.

One big limitation is that the keyboard mapping/remapping features that some
arcan appls like durden employ will not work here. The server only uses the
raw OS keycodes mapped into the sanity absorbing black chasm that is Xkb. Use
the normal Xserver arguments to pick whatever layout you need.

Issues
====
There is still lots to do. The reasons Glamor/GLX are marked as p and not x
right now:

* GLX seem to pick software only here (0ad/glxgears really slow), might
  be my MESA build/config but not sure. If not, there's failures (crash amdgpu)
	and missing opc on intel for 148:4 and 148:2.

* Lifecycle management during glamor, glx and randr resize is UAF prone, the
  chaining-modifying callback pScreen "API" style really doesn't help.

* glamor buffer ouput formats seem to mismatch (swapped R/B channels with
  broken alpha), can be worked around shader-side, but it should be fixed.
	Somewhat weird that the format we get out from the bo seem to mismatch
	on the other side, mesa bug?

TODO
====
There are still a number of TODOs before all X clients can be successfully
bridged, here are the current big points:
(x - done, p - partial)

- [x] Damage- Regions
- [x] Randr/DISPLAYHINT resizing
- [ ] Touch Input Mapping
- [ ] Joystick Input Mapping
- [ ] Gamma Control Bridging
- [ ] Display-correct Synchronization timing
  - [ ] DRI2-shmif-framebuffer-GL handle mapping
- [ ] Accelerated Cursor
- [p] Glamor/dri3
- [p] GLX
- [ ] epoxy patching (or switch gl calls to use agp-fenv)
    - [ ] overridable lookup function (map to shmifext-)
    - [ ] invalidate / replace
- [ ] Gamma Translation
- [ ] Clipboard Integration
    - [ ] Pasteboard
    - [ ] Text
    - [ ] Bchunk
    - [ ] Video
