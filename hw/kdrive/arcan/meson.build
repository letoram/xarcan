srcs = [
    'core.c',
    'init.c',
    'cvt.c',
    'proxywnd.c',
    'clipboard.c',
    'cursor.c',
    'present.c'
]

arcan_dep = [
    common_dep,
    gbm_dep,
    libdrm_dep,
    epoxy_dep,
    dependency('arcan-shmif'),
    dependency('arcan-shmif-ext'),
    dependency('xcb'),
    dependency('xcb-icccm'),
    dependency('xcb-xfixes'),
    dependency('xcb-util')
]

arcan_server = executable(
    'Xarcan',
    srcs,
    include_directories: [
        inc,
        include_directories('../src')
    ],
    dependencies: arcan_dep,
    link_with: [
        libxserver_main,
        kdrive,
        glamor,
        libxserver_fb,
        libxserver,
        libxserver_config,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd
    ],
    install: true,
)
