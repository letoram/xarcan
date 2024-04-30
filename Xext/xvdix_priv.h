/* SPDX-License-Identifier: MIT OR X11
 *
 * Copyright © 2024 Enrico Weigelt, metux IT consult <info@metux.net>
 */
#ifndef _XORG_XVDIX_PRIV_H

#include <X11/Xdefs.h>

#include "Xext/xvdix.h"

#define VALIDATE_XV_PORT(portID, pPort, mode)\
    {\
        int rc = dixLookupResourceByType((void **)&(pPort), portID,\
                                         XvRTPort, client, mode);\
        if (rc != Success)\
            return rc;\
    }

/* Errors */

#define _XvBadPort (XvBadPort+XvErrorBase)

extern int XvReqCode;
extern int XvErrorBase;

extern RESTYPE XvRTPort;

#endif /* _XORG_XVDIX_PRIV_H */
