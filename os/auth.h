#ifndef _XSERVER_OS_AUTH_H
#define _XSERVER_OS_AUTH_H

#include <X11/X.h>

#include "dix.h"

#define AuthInitArgs void
typedef void (*AuthInitFunc) (AuthInitArgs);

#define AuthAddCArgs unsigned short data_length, const char *data, XID id
typedef int (*AuthAddCFunc) (AuthAddCArgs);

#define AuthCheckArgs unsigned short data_length, const char *data, ClientPtr client, const char **reason
typedef XID (*AuthCheckFunc) (AuthCheckArgs);

#define AuthFromIDArgs XID id, unsigned short *data_lenp, char **datap
typedef int (*AuthFromIDFunc) (AuthFromIDArgs);

#define AuthGenCArgs unsigned data_length, const char *data, XID id, unsigned *data_length_return, char **data_return
typedef XID (*AuthGenCFunc) (AuthGenCArgs);

#define AuthRemCArgs unsigned short data_length, const char *data
typedef int (*AuthRemCFunc) (AuthRemCArgs);

#define AuthRstCArgs void
typedef int (*AuthRstCFunc) (AuthRstCArgs);

int set_font_authorizations(char **authorizations,
                            int *authlen,
                            void *client);

#define LCC_UID_SET     (1 << 0)
#define LCC_GID_SET     (1 << 1)
#define LCC_PID_SET     (1 << 2)
#define LCC_ZID_SET     (1 << 3)

typedef struct {
    int fieldsSet;              /* Bit mask of fields set */
    int euid;                   /* Effective uid */
    int egid;                   /* Primary effective group id */
    int nSuppGids;              /* Number of supplementary group ids */
    int *pSuppGids;             /* Array of supplementary group ids */
    int pid;                    /* Process id */
    int zoneid;                 /* Only set on Solaris 10 & later */
} LocalClientCredRec;

int GetLocalClientCreds(ClientPtr, LocalClientCredRec **);
void FreeLocalClientCreds(LocalClientCredRec *);

#endif /* _XSERVER_OS_AUTH_H */
