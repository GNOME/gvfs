/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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

#ifndef __G_VFS_FTP_DIRCACHE_H__
#define __G_VFS_FTP_DIRCACHE_H__

#include <gvfsftpfile.h>
#include <gvfsftptask.h>

G_BEGIN_DECLS


//typedef struct _GVfsFtpDirCache GVfsFtpDirCache;
typedef struct _GVfsFtpDirCacheEntry GVfsFtpDirCacheEntry;
//typedef struct _GVfsFtpDirFuncs GVfsFtpDirFuncs;

struct _GVfsFtpDirFuncs {
  const char *          command;
  gboolean              (* process)                             (GInputStream *         stream,
                                                                 int                    debug_id,
                                                                 const GVfsFtpFile *    dir,
                                                                 GVfsFtpDirCacheEntry * entry,
                                                                 GCancellable *         cancellable,
                                                                 GError **              error);
  GFileInfo *           (* lookup_uncached)                     (GVfsFtpTask *          task,
                                                                 const GVfsFtpFile *    file);
  GVfsFtpFile *         (* resolve_symlink)                     (GVfsFtpTask *          task,
                                                                 const GVfsFtpFile *    file,
                                                                 const char *           target);
};

extern const GVfsFtpDirFuncs g_vfs_ftp_dir_cache_funcs_unix;
extern const GVfsFtpDirFuncs g_vfs_ftp_dir_cache_funcs_default;

GVfsFtpDirCache *       g_vfs_ftp_dir_cache_new                 (const GVfsFtpDirFuncs *funcs);
void                    g_vfs_ftp_dir_cache_free                (GVfsFtpDirCache *      cache);

GFileInfo *             g_vfs_ftp_dir_cache_lookup_file         (GVfsFtpDirCache *      cache,
                                                                 GVfsFtpTask *          task,
                                                                 const GVfsFtpFile *    file,
                                                                 gboolean               resolve_symlinks);
GList *                 g_vfs_ftp_dir_cache_lookup_dir          (GVfsFtpDirCache *      cache,
                                                                 GVfsFtpTask *          task,
                                                                 const GVfsFtpFile *    dir,
                                                                 gboolean               flush,
                                                                 gboolean               resolve_symlinks);
void                    g_vfs_ftp_dir_cache_purge_file          (GVfsFtpDirCache *      cache,
                                                                 const GVfsFtpFile *    file);
void                    g_vfs_ftp_dir_cache_purge_dir           (GVfsFtpDirCache *      cache,
                                                                 const GVfsFtpFile *    dir);

void                    g_vfs_ftp_dir_cache_entry_add           (GVfsFtpDirCacheEntry * entry,
                                                                 GVfsFtpFile *          file,
                                                                 GFileInfo *            info);


G_END_DECLS

#endif /* __G_VFS_FTP_DIRCACHE_H__ */
