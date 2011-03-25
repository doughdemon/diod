/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  Written by Jim Garlick <garlick@llnl.gov> LLNL-CODE-423279
 *  All Rights Reserved.
 *
 *  This file is part of the Distributed I/O Daemon (diod).
 *  For details, see <http://code.google.com/p/diod/>.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* diod_upool.c - user lookup for diod distributed I/O daemon */

/* This upool implementation does no caching of /etc/passwd and /etc/group.
 * Instead it creates Npusers and Npgroups anew at attach time, shares them
 * among cloned fids, and deletes them when the refcount goes to zero
 * (e.g. when root is clunked at umount time).  A users supplementary groups
 * are not stored in Npuser->groups; instead supplementary groups are stored
 * a private struct linked to Npuser->aux in the form of an array suitable
 * for passing directly to setgroups().
 *
 * Summary of refcount strategy:  
 * Tattach: created via diod_u*2user() with refcount 1 (see below)
 * Twalk: if cloning the fid, refcount++ 
 * Tclunk: if fid is destroyed, refcount--
 * User is destroyed when its refcount reaches 0.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <assert.h>

#include "9p.h"
#include "npfs.h"

#include "list.h"
#include "diod_conf.h"
#include "diod_log.h"
#include "diod_upool.h"

#ifndef NGROUPS_MAX
#define NGROUPS_MAX     256  /* FIXME: sysconf(_SC_NGROUPS_MAX); */
#endif
#define PASSWD_BUFSIZE  4096 /* FIXME: sysconf(_SC_GETPW_R_SIZE_MAX) ? */
#define GROUP_BUFSIZE   4096 /* FIXME: sysconf(_SC_GETGR_R_SIZE_MAX) ? */
#define PATH_GROUP      "/etc/group"
#define SQUASH_UID      65534
#define SQUASH_GID      65534

static Npuser       *diod_uname2user (Npuserpool *, char *uname);
static Npuser       *diod_uid2user (Npuserpool *, u32 uid);
static void          diod_udestroy (Npuserpool *up, Npuser *u);

static Npgroup      *diod_gname2group(Npuserpool *, char *gname);
static Npgroup      *diod_gid2group(Npuserpool *, u32 gid);
static void          diod_gdestroy(Npuserpool *, Npgroup *);

static Npuserpool upool = {
    .uname2user     = diod_uname2user,
    .uid2user       = diod_uid2user,
    .gname2group    = diod_gname2group,
    .gid2group      = diod_gid2group,
    .ismember       = NULL,
    .udestroy       = diod_udestroy,
    .gdestroy       = diod_gdestroy,
};

/* Private Duser struct, linked to Npuser->aux.
 */
#define DUSER_MAGIC 0x3455DDDF
typedef struct {
    int         magic;
    int         nsg;            /* number of supplementary groups */
    gid_t       sg[NGROUPS_MAX];/* supplementary gid array */
} Duser;

Npuserpool *diod_upool = &upool;

/* Switch fsuid to user/group and load supplemental groups.
 * N.B. ../tests/misc/t00, ../tests/misc/t01, and ../tests/misc/t02
 * demonstrate that this works with pthreads work crew.
 */
int
diod_switch_user (Npuser *u, gid_t gid_override)
{
    Duser *d = u->aux;
    int ret = 0;

    assert (d->magic == DUSER_MAGIC);

    if (diod_conf_opt_runasuid ()) /* bail early if running as one user */
        return 1;

    if (setgroups (d->nsg, d->sg) < 0) {
        np_uerror (errno);
        goto done;
    }
    if (setfsgid (gid_override == -1 ? u->dfltgroup->gid : gid_override) < 0) {
        np_uerror (errno);
        goto done;
    }
    if (setfsuid (u->uid) < 0) {
        np_uerror (errno);
        goto done;
    }
    ret = 1;
done:
    return ret;
}

int
_getsg (struct passwd *pwd, gid_t *gp, int *glenp)
{
    FILE *f;
    int i, err;
    struct group grp, *grpp;
    char buf[GROUP_BUFSIZE];
   
    *glenp = 0; 
    if (!(f = fopen (PATH_GROUP, "r")))
        return 0;
    while ((err = fgetgrent_r (f, &grp, buf, sizeof (buf), &grpp)) == 0) {
        for (i = 0; grpp->gr_mem[i] != NULL; i++) {
            if (strcmp (grpp->gr_mem[i], pwd->pw_name) == 0) {
                if (*glenp < NGROUPS_MAX) {
                    if (grpp->gr_gid != pwd->pw_gid)
                        gp[(*glenp)++] = grpp->gr_gid;
                } else
                    msg ("user %s exceeded supplementary group max of %d",
                         pwd->pw_name, NGROUPS_MAX);
                break;
            }
        }
    } 
    fclose (f);
    return 1;
}

static Duser *
_alloc_duser (struct passwd *pwd)
{
    Duser *d = NULL;

    if (!(d = np_malloc (sizeof (*d))))
        goto done;
    d->magic = DUSER_MAGIC;
    if (!_getsg (pwd, d->sg, &d->nsg))
        np_uerror (errno);
done:
    if (np_rerror () && d != NULL) {
        free (d);
        d = NULL;
    }
    return d;
}

/* Switch to user/group, load the user's supplementary groups.
 * Print message and exit on failure.
 */
void
diod_become_user (char *name, uid_t uid, int realtoo)
{
    int err;
    struct passwd pw, *pwd;
    char buf[PASSWD_BUFSIZE];
    int nsg;
    gid_t sg[NGROUPS_MAX];
    char *endptr;

    if (name) {
        errno = 0;
        uid = strtoul (name, &endptr, 10);
        if (errno == 0 && *name != '\0' && *endptr == '\0')
            name = NULL;
    }
    if (name) {
        if ((err = getpwnam_r (name, &pw, buf, sizeof(buf), &pwd)) != 0)
            errn_exit (err, "error looking up user %s", name);
        if (!pwd)
            msg_exit ("error looking up user %s", name);
    } else {
        if ((err = getpwuid_r (uid, &pw, buf, sizeof(buf), &pwd)) != 0)
            errn_exit (err, "error looking up uid %d", uid);
        if (!pwd)
            msg_exit ("error looking up uid %d", uid);
    }
    if (!_getsg (pwd, sg, &nsg))
        err_exit (PATH_GROUP); 
    if (setgroups (nsg, sg) < 0)
        err_exit ("setgroups");
    if (setregid (realtoo ? pwd->pw_gid : -1, pwd->pw_gid) < 0)
        err_exit ("setreuid");
    if (setreuid (realtoo ? pwd->pw_uid : -1, pwd->pw_uid) < 0)
        err_exit ("setreuid");
}


static Npuser *
_alloc_user (Npuserpool *up, struct passwd *pwd)
{
    Npuser *u;
    int err;

    if (!(u = np_malloc (sizeof (*u))))
        goto done;
    if (!(u->aux = _alloc_duser (pwd)))
        goto done;
    if ((err = pthread_mutex_init (&u->lock, NULL)) != 0) {
        np_uerror (err);
        goto done;
    }
    u->refcount = 0;
    u->upool = up;
    u->uid = pwd->pw_uid;
    if (!(u->uname = strdup (pwd->pw_name))) {
        np_uerror (ENOMEM);
        goto done;
    }

    u->dfltgroup = up->gid2group(up, pwd->pw_gid);
    u->groups = NULL;       /* not used */
    u->ngroups = 0;         /* not used */
    u->next = NULL;         /* not used */
done:
    if (np_rerror () && u != NULL) {
        if (u->aux)
            free (u->aux);
        if (u->uname)
            free (u->uname);
        free (u);
        u = NULL;
    }
    return u; 
}

/* Called from npfs/libnpfs/user.c::np_user_decref ().
 */
static void
diod_udestroy (Npuserpool *up, Npuser *u)
{
    if (u->aux)
        free (u->aux);
    if (u->uname)
        free (u->uname);
    /* caller frees u */
}

/* N.B. This (or diod_uid2user) is called when handling a 9P attach message.
 * Also called when building a pseudo file system as in diodctl/ops.c.
 */
static Npuser *
diod_uname2user (Npuserpool *up, char *uname)
{
    Npuser *u = NULL; 
    int err;
    struct passwd pw, *pwd = NULL;
    char buf[PASSWD_BUFSIZE];

    if ((err = getpwnam_r (uname, &pw, buf, sizeof(buf), &pwd)) != 0) {
        np_uerror (err);
        goto done;
    }
    if (!pwd) {
        np_uerror (ESRCH);
        goto done;
    }
    if (!(u = _alloc_user (up, pwd)))
        goto done;
    np_user_incref (u);
done:
    return u;
}

/* This (or diod_uname2user) is called when handling a 9P attach message.
 * Also called when building a pseudo file system as in diodctl/ops.c.
 */
static Npuser *
diod_uid2user(Npuserpool *up, u32 uid)
{
    Npuser *u = NULL; 
    int err;
    struct passwd pw, *pwd;
    char buf[PASSWD_BUFSIZE];

    if ((err = getpwuid_r (uid, &pw, buf, sizeof(buf), &pwd)) != 0) {
        np_uerror (err);
        goto done;
    }
    if (!pwd) {
        np_uerror (ESRCH);
        goto done;
    }
    if (!(u = _alloc_user (up, pwd)))
        goto done;
done:
    np_user_incref (u);
    return u;
}

static Npgroup *
_alloc_group (Npuserpool *up, struct group *gr)
{
    Npgroup *g;
    int err;

    if (!(g = np_malloc (sizeof (*g))))
        goto done;
    if ((err = pthread_mutex_init (&g->lock, NULL)) != 0) {
        np_uerror (err);
        goto done;
    }
    g->refcount = 0;
    g->upool = up;
    g->gid = gr->gr_gid;
    if (!(g->gname = strdup (gr->gr_name))) {
        np_uerror (ENOMEM);
        goto done;
    }
    g->aux = NULL;          /* not used */
    g->next = NULL;         /* not used */
done:
    if (np_rerror () && g != NULL) {
        if (g->gname)
            free (g->gname);
        free (g);
        g = NULL;
    }
    return g; 

}

static Npgroup *
diod_gname2group(Npuserpool *up, char *gname)
{
    Npgroup *g = NULL; 
    int err;
    struct group gr, *grp = NULL;
    char buf[GROUP_BUFSIZE];

    if ((err = getgrnam_r (gname, &gr, buf, sizeof(buf), &grp)) != 0) {
        np_uerror (err);
        goto done;
    }
    if (!grp) {
        np_uerror (ESRCH);
        goto done;
    }
    if (!(g = _alloc_group (up, grp)))
        goto done;
    np_group_incref (g);
done:
    return g;
}

static Npgroup *
diod_gid2group(Npuserpool *up, u32 gid)
{
    Npgroup *g = NULL; 
    int err;
    struct group gr, *grp = NULL;
    char buf[GROUP_BUFSIZE];

    if ((err = getgrgid_r (gid, &gr, buf, sizeof(buf), &grp)) != 0) {
        np_uerror (err);
        goto done;
    }
    if (!grp) {
        np_uerror (ESRCH);
        goto done;
    }
    if (!(g = _alloc_group (up, grp)))
        goto done;
    np_group_incref (g);
done:
    return g;
}

static void
diod_gdestroy(Npuserpool *up, Npgroup *g)
{
    if (g->gname)
        free (g->gname);
    /* caller frees g */
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
