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
    --disable-xnest --disable-xvfb --enable-glamor --enable-glx

pkg\_info need to find the normal arcan-shmif, arcan-shmifext libraries.

Though a ton of other X features, could/should also be disabled.
If you find a better set of compilation options for a feature- complete yet
tiny X server, let me know.

Note that keyboard layouts and fonts are searched on based on the build
prefix (like --prefix=/usr), and xfonts2 may need to be built from source
on some distributions.

Use
====
From a terminal with access to connection primitives (env: ARCAN\_CONNPATH),
just run hw/kdrive/arcan/Xarcan to start bridging connections.

For multiple instances, you can add :1 (and set DISPLAY=:1 etc. accordingly)
and so one, forcing size with -screen 800x600 -no-dynamic and enable glamor
(GL rendering with handle passing) using -glamor (see issues). The title and
identity can be scriptably controlled with -aident identstr -atitle titlestr

Clipboard management is not part of xarcan per se, there is an external tool
in the arcan source distribution (src/tools/aclip) that can be paired with
managers like xclip to bridge clipboard management between arcan instances
with appls that support it and the Xserver.

Limitations
====
The strategy used here is to contain all X clients within one logical window,
for mapping single- client windows to corresponding Arcan primitives, the plan
is still to go through Wayland/XWayland, though that is still somewhat
uncertain, investigations are done on a hybrid version where we can 'steal'
single windows out from Xorg on demand. Another exception is if a DRI3 client
goes full-screen, then the buffer we passed should switch to that one
immediately.

One big limitation is that the keyboard mapping/remapping features that some
arcan appls like durden employ will not work here. The server only uses the
raw OS keycodes mapped into the sanity absorbing black chasm that is Xkb.
Since the same problem exist for bridge wayland clients, chances are that
something more dynamic can be obtained via the github.com/39aldo39/klfc util
as a middle man.

Issues
====
There is still some things left to do. Quite a few of the accelerated graphics
problems pertain to having multiple GPUs or having Xarcan working against a
render node rather than the real device.

This can be mitigated by pointing the ARCAN\_RENDER\_NODE environment variable
to a card-node rather than a render node for starters. This also requires that
the xarcan- connection to the running appl- scripts in arcan gets the GPU-
delegation flag (durden: global/config/system/gpu delegation=full) as it breaks
the privilege separation initiative from using render nodes in the first place.

Notes
====
(might be wildly incorrect)

There seem to be some weird path/trick needed to be performed for the PRESENT-
extension to be enabled, the full path is yet to be traced, but when working,
it should be a mere matter of communicating timings and switching to a signal
without block- ignore.

The 'Output Segment' todo is a possibly neat way to solve compatibility/mapping
for clients that requests the contents of other windows. The plan is to activate
when a NEWSEGMENT event arrives with an output segment (i.e. an explicit push).

When that happens, take the pScreen and overload getImage to map/return contents
from that segment rather than somewhere else.

TODO
====
There are still a number of TODOs before all X clients can be successfully
bridged, here are the current big points:
(x - done, p - partial)

- [x] Damage- Regions
- [x] Gamma Control Bridging
- [x] Randr/DISPLAYHINT resizing
- [ ] Touch Input Mapping
- [ ] Joystick Input Mapping
- [ ] Output Segment to universal GetImage
- [ ] mouse cursor acceleration
- [ ] Display-correct Synchronization timing
  - [p] PRESENT- support
- [p] Accelerated Cursor
- [x] Glamor/dri3/glx
- [ ] epoxy patching (or switch gl calls to use agp-fenv)
    - [ ] overridable lookup function (map to shmifext-)
    - [ ] invalidate / replace
- [ ] xenocara bringup
