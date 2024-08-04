/***********************************************************

Copyright 1987, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.

Copyright 1987 by Digital Equipment Corporation, Maynard, Massachusetts.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Digital not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

******************************************************************/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifndef _OSDEP_H_
#define _OSDEP_H_ 1

#include <X11/Xdefs.h>

#if defined(XDMCP) || defined(HASXDMAUTH)
#include <X11/Xdmcp.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <X11/Xos.h>
#include <X11/Xmd.h>
#include <X11/Xdefs.h>

/* If EAGAIN and EWOULDBLOCK are distinct errno values, then we check errno
 * for both EAGAIN and EWOULDBLOCK, because some supposedly POSIX
 * systems are broken and return EWOULDBLOCK when they should return EAGAIN
 */
#ifndef WIN32
# if (EAGAIN != EWOULDBLOCK)
#  define ETEST(err) (err == EAGAIN || err == EWOULDBLOCK)
# else
#  define ETEST(err) (err == EAGAIN)
# endif
#else   /* WIN32 The socket errorcodes differ from the normal errors */
#define ETEST(err) (err == EAGAIN || err == WSAEWOULDBLOCK)
#endif

#if defined(XDMCP) || defined(HASXDMAUTH)
typedef Bool (*ValidatorFunc) (ARRAY8Ptr Auth, ARRAY8Ptr Data, int packet_type);
typedef Bool (*GeneratorFunc) (ARRAY8Ptr Auth, ARRAY8Ptr Data, int packet_type);
typedef Bool (*AddAuthorFunc) (unsigned name_length, const char *name,
                               unsigned data_length, char *data);
#endif

typedef struct _connectionInput *ConnectionInputPtr;
typedef struct _connectionOutput *ConnectionOutputPtr;

struct _osComm;

typedef int (*OsFlushFunc) (ClientPtr who, struct _osComm * oc, char *extraBuf,
                            int extraCount);

typedef struct _osComm {
    int fd;
    ConnectionInputPtr input;
    ConnectionOutputPtr output;
    XID auth_id;                /* authorization id */
    CARD32 conn_time;           /* timestamp if not established, else 0  */
    struct _XtransConnInfo *trans_conn; /* transport connection object */
    int flags;
} OsCommRec, *OsCommPtr;

#define OS_COMM_GRAB_IMPERVIOUS 1
#define OS_COMM_IGNORED         2

extern int FlushClient(ClientPtr /*who */ ,
                       OsCommPtr /*oc */ ,
                       const void * /*extraBuf */ ,
                       int      /*extraCount */
    );

extern void FreeOsBuffers(OsCommPtr     /*oc */
    );

void
CloseDownFileDescriptor(OsCommPtr oc);

#include "dix.h"
#include "ospoll.h"

extern struct ospoll    *server_poll;

Bool
listen_to_client(ClientPtr client);

extern Bool NewOutputPending;

/* in access.c */
extern Bool ComputeLocalClient(ClientPtr client);

/* in auth.c */
extern void GenerateRandomData(int len, char *buf);

/* OsTimer functions */
void TimerInit(void);
Bool TimerForce(OsTimerPtr timer);

#ifdef WIN32
#include <X11/Xwinsock.h>
struct utsname {
    char nodename[512];
};

static inline void uname(struct utsname *uts) {
    gethostname(uts->nodename, sizeof(uts->nodename));
}

const char *Win32TempDir(void);

int System(const char *cmdline);
static inline void Fclose(void *f) { fclose(f); }
static inline void *Fopen(const char *a, const char *b) { return fopen(a,b); }

#else /* WIN32 */

int System(const char *);
void *Popen(const char *, const char *);
void *Fopen(const char *, const char *);
int Fclose(void *f);
int Pclose(void *f);

#endif /* WIN32 */

void AutoResetServer(int sig);

/* clone fd so it gets out of our select mask */
int os_move_fd(int fd);

/* set signal mask - either on current thread or whole process,
   depending on whether multithreading is used */
int xthread_sigmask(int how, const sigset_t *set, sigset_t *oldest);

/* callback for DDX specific error printing, if any (may be NULL) */
extern void (*OsVendorVErrorFProc) (const char *, va_list args)
    _X_ATTRIBUTE_PRINTF(1, 0);

typedef void (*OsSigHandlerPtr) (int sig);

/* install signal handler */
OsSigHandlerPtr OsSignal(int sig, OsSigHandlerPtr handler);

void OsInit(void);
void OsCleanup(Bool);
void OsVendorFatalError(const char *f, va_list args) _X_ATTRIBUTE_PRINTF(1, 0);
void OsVendorInit(void);
void OsBlockSignals(void);
void OsReleaseSignals(void);
void OsResetSignals(void);
void OsAbort(void) _X_NORETURN;

void MakeClientGrabPervious(ClientPtr client);
void MakeClientGrabImpervious(ClientPtr client);

int OnlyListenToOneClient(ClientPtr client);

void ListenToAllClients(void);

/* allow DDX to force using another clock */
void ForceClockId(clockid_t forced_clockid);

extern int LimitClients;
extern Bool PartialNetwork;
extern Bool RunFromSigStopParent;

extern int limitDataSpace;
extern int limitStackSpace;
extern int limitNoFile;

extern Bool CoreDump;
extern Bool NoListenAll;
extern Bool AllowByteSwappedClients;

#endif                          /* _OSDEP_H_ */
