/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 */

/* prototypes for the os-support layer of xfree86 DDX */

#ifndef _XSERVER_XF86_OS_SUPPORT
#define _XSERVER_XF86_OS_SUPPORT

#include <X11/Xdefs.h>

typedef void (*PMClose) (void);

void xf86OpenConsole(void);
void xf86CloseConsole(void);
Bool xf86VTActivate(int vtno);
Bool xf86VTSwitchPending(void);
Bool xf86VTSwitchAway(void);
Bool xf86VTSwitchTo(void);
void xf86VTRequest(int sig);
int xf86ProcessArgument(int argc, char **argv, int i);
void xf86UseMsg(void);
PMClose xf86OSPMOpen(void);
void xf86InitVidMem(void);

void xf86OSRingBell(int volume, int pitch, int duration);

#endif /* _XSERVER_XF86_OS_SUPPORT */
