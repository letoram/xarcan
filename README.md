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

* glamor buffer output formats seems broken in an interesting way, the
  contents are correct except for the alpha channel that is 'mostly' all 0.
	Can workaround in durden with target/video/shader/no_alpha right now.
	Unsure if this is deep in the bowels of X or MESA.

* if it is unsuably unstable, try running with -nodynamic

* glamor with DRI3 gives interesting client crashes when the X server itself
  is bound to a render-node rather than a card (at least on amdgpu), running
	with -nodri3 works, but chances are you'll get the software path instead.

* resource management / cleanup is extremely error prone. Xorg uses a volatile
  pattern of structures with callbacks that you attach-chain to, meaning that
	execution goes all over the place and it's a pain to trace. Whenever someone
	incorrectly chains or unchains, you get backtraces that are awful to figure
	out.

Notes
====
(might be wildly incorrect)

It seems possible that the 2D rendering synchronization issues can be improved
by switching to PRESENT and stop abusing the block handler.

The RandR use seem to need more CRTC/Fake display information in order for
both PRESENT and RRGetGamma/RRSetGamma to be invoked, and we need access to
the gamma functions if the shmif-cont is negotiated to work with that.

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
