/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Benjamin Otte <otte@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Benjamin Otte <otte@gnome.org>
 */

#ifndef __G_VFS_BACKEND_FTP_H__
#define __G_VFS_BACKEND_FTP_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_FTP_TIMEOUT_IN_SECONDS 30

typedef enum {
  G_VFS_FTP_FEATURE_MDTM,
  G_VFS_FTP_FEATURE_SIZE,
  G_VFS_FTP_FEATURE_TVFS,
  G_VFS_FTP_FEATURE_EPRT,
  G_VFS_FTP_FEATURE_EPSV,
  G_VFS_FTP_FEATURE_UTF8
} GVfsFtpFeature;
#define G_VFS_FTP_FEATURES_DEFAULT (0)

typedef enum {
  G_VFS_FTP_SYSTEM_UNKNOWN = 0,
  G_VFS_FTP_SYSTEM_UNIX,
  G_VFS_FTP_SYSTEM_WINDOWS
} GVfsFtpSystem;

typedef enum {
  G_VFS_FTP_METHOD_ANY = 0,
  G_VFS_FTP_METHOD_EPSV,
  G_VFS_FTP_METHOD_PASV,
  G_VFS_FTP_METHOD_PASV_ADDR,
  G_VFS_FTP_METHOD_EPRT,
  G_VFS_FTP_METHOD_PORT
} GVfsFtpMethod;

typedef enum {
  /* server does not allow querying features before login, so we try after
   * logging in instead. */
  G_VFS_FTP_WORKAROUND_FEAT_AFTER_LOGIN,
} GVfsFtpWorkaround;

/* forward declarations */
typedef struct _GVfsFtpDirCache GVfsFtpDirCache;
typedef struct _GVfsFtpDirFuncs GVfsFtpDirFuncs;

#define G_VFS_TYPE_BACKEND_FTP         (g_vfs_backend_ftp_get_type ())
#define G_VFS_BACKEND_FTP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtp))
#define G_VFS_BACKEND_FTP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtpClass))
#define G_VFS_IS_BACKEND_FTP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_FTP))
#define G_VFS_IS_BACKEND_FTP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_FTP))
#define G_VFS_BACKEND_FTP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtpClass))

typedef struct _GVfsBackendFtp        GVfsBackendFtp;
typedef struct _GVfsBackendFtpClass   GVfsBackendFtpClass;

struct _GVfsBackendFtp
{
  GVfsBackend           backend;

  GSocketConnectable *  addr;
  GSocketClient *       connection_factory;
  char *                user;
  gboolean              has_initial_user;
  char *                password;	        /* password or NULL for anonymous */
  char *                host_display_name;

  GVfsFtpSystem         system;                 /* the system from the SYST response */
  int                   features;               /* GVfsFtpFeatures that are supported */
  int                   workarounds;            /* GVfsFtpWorkarounds in use - int because it's atomic */
  int                   method;                 /* preferred GVfsFtpMethod - int because it's atomic */

  /* directory cache */
  const GVfsFtpDirFuncs *dir_funcs;             /* functions used in directory cache */
  GVfsFtpDirCache *     dir_cache;              /* directory cache */

  /* connection collection - accessed from gvfsftptask.c */
  GMutex *              mutex;                  /* mutex protecting the following variables */
  GCond *               cond;                   /* cond used to signal tasks waiting on the mutex */
  GQueue *              queue;                  /* queue containing the connections */
  guint                	connections;            /* current number of connections */
  guint                 busy_connections;       /* current number of connections being used for reads/writes */
  guint                	max_connections;        /* upper server limit for number of connections - dynamically generated */
};

struct _GVfsBackendFtpClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_ftp_get_type (void) G_GNUC_CONST;

gboolean        g_vfs_backend_ftp_has_feature           (GVfsBackendFtp *       ftp,
                                                         GVfsFtpFeature         feature);
gboolean        g_vfs_backend_ftp_uses_workaround       (GVfsBackendFtp *       ftp,
                                                         GVfsFtpWorkaround      workaround);
void            g_vfs_backend_ftp_use_workaround        (GVfsBackendFtp *       ftp,
                                                         GVfsFtpWorkaround      workaround);


G_END_DECLS

#endif /* __G_VFS_BACKEND_FTP_H__ */
