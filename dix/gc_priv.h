/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 * Copyright © 1987 by Digital Equipment Corporation, Maynard, Massachusetts.
 * Copyright © 1987, 1998  The Open Group
 */
#ifndef _XSERVER_DIX_GC_PRIV_H
#define _XSERVER_DIX_GC_PRIV_H

#include "include/gc.h"

int ChangeGCXIDs(ClientPtr client, GCPtr pGC, BITS32 mask, CARD32 * pval);

GCPtr CreateGC(DrawablePtr pDrawable,
               BITS32 mask,
               XID *pval,
               int *pStatus,
               XID gcid,
               ClientPtr client);

int CopyGC(GCPtr pgcSrc, GCPtr pgcDst, BITS32 mask);

int FreeGC(void *pGC, XID gid);

#endif /* _XSERVER_DIX_GC_PRIV_H */
