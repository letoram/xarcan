#ifndef _ARCANDEV_H_
#define _ARCANDEV_H_

#include <stdio.h>
#include <unistd.h>
#include <arcan_shmif.h>

#include "kdrive.h"
#include "damage.h"
#include "hashtable.h"

#ifdef RANDR
#include "randrstr.h"
#endif

struct proxyWindowData {
    int socket;
    size_t w, h;
    ssize_t x, y;
    uint32_t arcan_vid;
    bool paste;
    struct arcan_shmif_cont *cont;
};

struct proxyMapEntry {
    uint32_t vid;
};

typedef struct _arcanPriv {
    CARD8 *base;
    int bytes_per_line;
} arcanPriv;

typedef struct _arcanWindowPriv {
    arcan_event ev;
    arcan_event ident;
    DamagePtr damage;
    uint8_t mstate[ASHMIF_MSTATE_SZ];
    struct arcan_shmif_cont *shmif;
    Bool usePresent;
} arcanWindowPriv;

typedef struct _arcanPixmapPriv {
    int64_t id;           /* screen shmif-context cache group / index */
    struct gbm_bo *bo;    /* tracking for shmifext- accelerated transfers */
    void *image;          /* EGLImage reference handle */
    unsigned int texture; /* GL texture id reference */
    char *tmpbuf;
} arcanPixmapPriv;

typedef struct _arcanShmifPriv {
    PixmapPtr pixmap;
    WindowPtr window;
    bool bound, vblank, present;
    unsigned long long dying;
} arcanShmifPriv;

struct gbm_bo;
typedef struct _arcanScrPriv {
    struct arcan_shmif_cont *acon, *cursor;
    struct arcan_shmif_initial init;
    int windowCount;
    Bool dirty;
    Bool unsynched;
    Bool wmSynch;
    arcan_event cursor_event;
    Bool cursorRealized;
    int clip_mode;

    struct arcan_shmif_cont* pool[64];
    uint64_t redirectBitmap;
    Bool defaultRootless;

    struct xorg_list vblank_triggers;

#ifdef RANDR
    RROutputPtr randrOutput;
    RRCrtcPtr randrCrtc;
    struct ramp_block block;
#endif
    Rotation randr;

    struct {
        PositionWindowProcPtr positionWindow;
        DestroyWindowProcPtr destroyWindow;
        RestackWindowProcPtr restackWindow;
        RealizeWindowProcPtr realizeWindow;
        UnrealizeWindowProcPtr unrealizeWindow;
        ChangeWindowAttributesProcPtr changeWindow;
        ConfigNotifyProcPtr configureWindow;
        ResizeWindowProcPtr resizeWindow;
        CreateWindowProcPtr createWindow;
        MarkOverlappedWindowsProcPtr markOverlappedWindows;
        GetImageProcPtr getImage;
        SetWindowPixmapProcPtr setWindowPixmap;
        GetWindowPixmapProcPtr getWindowPixmap;
        InterposeDrawableSrcDstProcPtr interposeDrawableSrcDst;
        CreatePixmapProcPtr createPixmap;
        DestroyPixmapProcPtr destroyPixmap;

        ScreenBlockHandlerProcPtr screenBlockHandler;
        CreateScreenResourcesProcPtr createScreenResources;
        CloseScreenProcPtr closeScreen;
    } hooks;

    HashTable proxyMap;

    ScreenPtr screen;
    DamagePtr damage;
    Bool in_glamor;
    struct gbm_bo* bo;
    int tex;
    int pending_fd;
    CARD16 stride;
    CARD32 size;
    int format;
    Bool cursorUpdated;
} arcanScrPriv;

typedef struct _arcanInput {
    KdKeyboardInfo * ki;
    KdPointerInfo * pi;
    int wx, wy;
    uint64_t bmask;
} arcanInput;

typedef struct _arcanConfig {
    const char* title;
    const char* ident;
    Bool no_dri3;
    Bool glamor;
    Bool present;
    Bool no_dynamic_resize;
    Bool soft_mouse;
    Bool redirect;

/* tracking for -exec */
    const char* exec;
    int timeout;
    int clientShutdownBound;
    OsTimerPtr shutdownTimer;
} arcanConfig;

extern arcanInput arcanInputPriv;

extern arcanConfig arcanConfigPriv;

extern KdCardFuncs arcanFuncs;

Bool
 arcanInitialize(KdCardInfo * card, arcanPriv * priv);

Bool
 arcanCardInit(KdCardInfo * card);

Bool
 arcanScreenInit(KdScreenInfo * screen);

Bool
 arcanScreenInitialize(KdScreenInfo * screen, arcanScrPriv * scrpriv);

Bool
 arcanInitScreen(ScreenPtr pScreen);

Bool
 arcanFinishInitScreen(ScreenPtr pScreen);

Bool
 arcanCreateResources(ScreenPtr pScreen);

arcanWindowPriv *ensureArcanWndPrivate(WindowPtr wnd);
arcanWindowPriv *arcanWindowFromWnd(WindowPtr wnd);
arcanPixmapPriv *arcanPixmapFromPixmap(PixmapPtr wnd);

int
 arcanEventDispatch(struct arcan_shmif_cont*, arcanScrPriv*, arcan_event* ev, int64_t);

void
arcanFlushEvents(int fd, int, void* tag);

void
arcanFlushRedirectedEvents(int fd, int, void* tag);

int
arcanInit(void);

void
arcanFini(void);

void
 arcanScreenFini(KdScreenInfo * screen);

void
 arcanCardFini(KdCardInfo * card);

void
 arcanGetColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

void
 arcanPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs);

Bool
 arcanMapFramebuffer(KdScreenInfo * screen);

void *arcanWindowLinear(ScreenPtr pScreen,
                       CARD32 row,
                       CARD32 offset, int mode, CARD32 *size, void *closure);

void
 arcanNotifyReady(void);

void
 arcanForkExec(void);

void
 arcanSetScreenSizes(ScreenPtr pScreen);

Bool
 arcanUnmapFramebuffer(KdScreenInfo * screen);

Bool
 arcanSetShadow(ScreenPtr pScreen);

Bool
 arcanCreateColormap(ColormapPtr pmap);

#ifdef GLAMOR
Bool arcanGlamorInit(ScreenPtr screen);
void arcanGlamorEnable(ScreenPtr screen);
void arcanGlamorDisable(ScreenPtr screen);
void arcanGlamorFini(ScreenPtr screen);
#endif

void* arcanProxyWindowDispatch(struct proxyWindowData*);
void* arcanProxyContentWindowDispatch(struct proxyWindowData*);
void* arcanClipboardDispatch(struct proxyWindowData*);

/*
 * With RandR enabled, we treat the DISPLAYHINT events from parent
 * as a monitor- reconfigure.
 */
#ifdef RANDR
RRModePtr
arcan_cvt(int HDisplay, int VDisplay, float VRefresh, Bool Reduced,
             Bool Interlaced);
Bool
 arcanRandRGetInfo(ScreenPtr pScreen, Rotation * rotations);

Bool

arcanRandRSetConfig(ScreenPtr pScreen,
                   Rotation randr, int rate, RRScreenSizePtr pSize);
Bool
 arcanRandRInit(ScreenPtr pScreen);

#endif

void arcanSynchCursor(arcanScrPriv *scr, Bool softUpdate);

extern KdPointerDriver arcanPointerDriver;

extern KdKeyboardDriver arcanKeyboardDriver;

#endif                          /* _FBDEV_H_ */
