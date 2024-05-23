/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 1996 Thomas E. Dickey <dickey@clark.net>
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 */
#ifndef _XSERVER_EXTINIT_PRIV_H
#define _XSERVER_EXTINIT_PRIV_H

#include "extinit.h"

extern Bool noGEExtension;

void CompositeExtensionInit(void);
void DamageExtensionInit(void);
void DbeExtensionInit(void);
void DPMSExtensionInit(void);
void GEExtensionInit(void);
void GlxExtensionInit(void);
void PanoramiXExtensionInit(void);
void RRExtensionInit(void);
void RecordExtensionInit(void);
void RenderExtensionInit(void);
void ResExtensionInit(void);
void ScreenSaverExtensionInit(void);
void ShapeExtensionInit(void);
void ShmExtensionInit(void);
void SyncExtensionInit(void);
void XCMiscExtensionInit(void);
void SecurityExtensionInit(void);
void XFree86BigfontExtensionInit(void);
void BigReqExtensionInit(void);
void XFixesExtensionInit(void);
void XInputExtensionInit(void);
void XkbExtensionInit(void);
void SELinuxExtensionInit(void);
void XTestExtensionInit(void);
void XvExtensionInit(void);
void XvMCExtensionInit(void);
void dri3_extension_init(void);
void PseudoramiXExtensionInit(void);

#endif /* _XSERVER_EXTINIT_PRIV_H */
