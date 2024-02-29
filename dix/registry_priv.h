/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 */
#ifndef _XSERVER_DIX_REGISTRY_H
#define _XSERVER_DIX_REGISTRY_H

/*
 * Setup and teardown
 */
void dixResetRegistry(void);
void dixFreeRegistry(void);
void dixCloseRegistry(void);

#endif /* _XSERVER_DIX_REGISTRY_H */
