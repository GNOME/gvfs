/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_VFS_BACKEND_H__
#define __G_VFS_BACKEND_H__

#include <gio/gio.h>
#include <gvfsdaemon.h>
#include <gvfsjob.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND         (g_vfs_backend_get_type ())
#define G_VFS_BACKEND(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND, GVfsBackend))
#define G_VFS_BACKEND_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND, GVfsBackendClass))
#define G_VFS_IS_BACKEND(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND))
#define G_VFS_IS_BACKEND_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND))
#define G_VFS_BACKEND_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND, GVfsBackendClass))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GVfsBackend, g_object_unref)

typedef struct _GVfsBackendPrivate GVfsBackendPrivate;
typedef struct _GVfsBackendClass   GVfsBackendClass;

typedef struct _GVfsJobMount            GVfsJobMount;
typedef struct _GVfsJobUnmount          GVfsJobUnmount;
typedef struct _GVfsJobMountMountable   GVfsJobMountMountable;
typedef struct _GVfsJobUnmountMountable GVfsJobUnmountMountable;
typedef struct _GVfsJobStartMountable   GVfsJobStartMountable;
typedef struct _GVfsJobStopMountable    GVfsJobStopMountable;
typedef struct _GVfsJobPollMountable    GVfsJobPollMountable;
typedef struct _GVfsJobOpenForRead      GVfsJobOpenForRead;
typedef struct _GVfsJobOpenIconForRead  GVfsJobOpenIconForRead;
typedef struct _GVfsJobSeekRead         GVfsJobSeekRead;
typedef struct _GVfsJobCloseRead        GVfsJobCloseRead;
typedef struct _GVfsJobRead             GVfsJobRead;
typedef struct _GVfsJobOpenForWrite     GVfsJobOpenForWrite;
typedef struct _GVfsJobWrite            GVfsJobWrite;
typedef struct _GVfsJobSeekWrite        GVfsJobSeekWrite;
typedef struct _GVfsJobTruncate         GVfsJobTruncate;
typedef struct _GVfsJobCloseWrite       GVfsJobCloseWrite;
typedef struct _GVfsJobQueryInfo        GVfsJobQueryInfo;
typedef struct _GVfsJobQueryInfoRead    GVfsJobQueryInfoRead;
typedef struct _GVfsJobQueryInfoWrite   GVfsJobQueryInfoWrite;
typedef struct _GVfsJobQueryFsInfo      GVfsJobQueryFsInfo;
typedef struct _GVfsJobEnumerate        GVfsJobEnumerate;
typedef struct _GVfsJobSetDisplayName   GVfsJobSetDisplayName;
typedef struct _GVfsJobTrash            GVfsJobTrash;
typedef struct _GVfsJobDelete           GVfsJobDelete;
typedef struct _GVfsJobMakeDirectory    GVfsJobMakeDirectory;
typedef struct _GVfsJobMakeSymlink      GVfsJobMakeSymlink;
typedef struct _GVfsJobCopy             GVfsJobCopy;
typedef struct _GVfsJobMove             GVfsJobMove;
typedef struct _GVfsJobPush             GVfsJobPush;
typedef struct _GVfsJobPull             GVfsJobPull;
typedef struct _GVfsJobSetAttribute     GVfsJobSetAttribute;
typedef struct _GVfsJobQueryAttributes  GVfsJobQueryAttributes;
typedef struct _GVfsJobCreateMonitor    GVfsJobCreateMonitor;
typedef struct _GVfsJobError            GVfsJobError;

typedef gpointer GVfsBackendHandle;

struct _GVfsBackend
{
  GObject parent_instance;

  GVfsBackendPrivate *priv;
};

struct _GVfsBackendClass
{
  GObjectClass parent_class;

  /* vtable */

  /* These try_ calls should be fast and non-blocking, scheduling the i/o
   * operations async (or on a thread) or reading from cache.
   * Returning FALSE means "Can't do this now or async", which
   * means the non-try_ version will be scheduled in a worker
   * thread.
   * A NULL here means operation not supported 
   */

  void     (*unmount)           (GVfsBackend *backend,
				 GVfsJobUnmount *job,
				 GMountUnmountFlags flags,
                                 GMountSource *mount_source);
  gboolean (*try_unmount)       (GVfsBackend *backend,
				 GVfsJobUnmount *job,
				 GMountUnmountFlags flags,
                                 GMountSource *mount_source);
  void     (*mount)             (GVfsBackend *backend,
				 GVfsJobMount *job,
				 GMountSpec *mount_spec,
				 GMountSource *mount_source,
				 gboolean is_automount);
  gboolean (*try_mount)         (GVfsBackend *backend,
				 GVfsJobMount *job,
				 GMountSpec *mount_spec,
				 GMountSource *mount_source,
				 gboolean is_automount);
  void     (*mount_mountable)   (GVfsBackend *backend,
				 GVfsJobMountMountable *job,
				 const char *filename,
				 GMountSource *mount_source);
  gboolean (*try_mount_mountable)(GVfsBackend *backend,
				 GVfsJobMountMountable *job,
				 const char *filename,
				 GMountSource *mount_source);
  void     (*unmount_mountable) (GVfsBackend *backend,
				 GVfsJobUnmountMountable *job,
				 const char *filename,
				 GMountUnmountFlags flags,
                                 GMountSource *mount_source);
  gboolean (*try_unmount_mountable)(GVfsBackend *backend,
				    GVfsJobUnmountMountable *job,
				    const char *filename,
				    GMountUnmountFlags flags,
                                    GMountSource *mount_source);
  void     (*eject_mountable)   (GVfsBackend *backend,
				 GVfsJobUnmountMountable *job,
				 const char *filename,
				 GMountUnmountFlags flags,
                                 GMountSource *mount_source);
  gboolean (*try_eject_mountable)(GVfsBackend *backend,
				  GVfsJobUnmountMountable *job,
				  const char *filename,
				  GMountUnmountFlags flags,
                                  GMountSource *mount_source);
  void     (*open_for_read)     (GVfsBackend *backend,
				 GVfsJobOpenForRead *job,
				 const char *filename);
  gboolean (*try_open_for_read) (GVfsBackend *backend,
				 GVfsJobOpenForRead *job,
				 const char *filename);
  void     (*open_icon_for_read) (GVfsBackend *backend,
                                  GVfsJobOpenIconForRead *job,
                                  const char *icon_id);
  gboolean (*try_open_icon_for_read) (GVfsBackend *backend,
                                      GVfsJobOpenIconForRead *job,
                                      const char *icon_id);
  void     (*close_read)        (GVfsBackend *backend,
				 GVfsJobCloseRead *job,
				 GVfsBackendHandle handle);
  gboolean (*try_close_read)    (GVfsBackend *backend,
				 GVfsJobCloseRead *job,
				 GVfsBackendHandle handle);
  void     (*read)              (GVfsBackend *backend,
				 GVfsJobRead *job,
				 GVfsBackendHandle handle,
				 char *buffer,
				 gsize bytes_requested);
  gboolean (*try_read)          (GVfsBackend *backend,
				 GVfsJobRead *job,
				 GVfsBackendHandle handle,
				 char *buffer,
				 gsize bytes_requested);
  void     (*seek_on_read)      (GVfsBackend *backend,
				 GVfsJobSeekRead *job,
				 GVfsBackendHandle handle,
				 goffset    offset,
				 GSeekType  type);
  gboolean (*try_seek_on_read)  (GVfsBackend *backend,
				 GVfsJobSeekRead *job,
				 GVfsBackendHandle handle,
				 goffset    offset,
				 GSeekType  type);
  gboolean (*try_create)        (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 GFileCreateFlags flags);
  void     (*create)            (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 GFileCreateFlags flags);
  gboolean (*try_append_to)     (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 GFileCreateFlags flags);
  void     (*append_to)         (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 GFileCreateFlags flags);
  gboolean (*try_replace)       (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 const char *etag,
				 gboolean make_backup,
				 GFileCreateFlags flags);
  void     (*replace)           (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 const char *etag,
				 gboolean make_backup,
				 GFileCreateFlags flags);
  gboolean (*try_edit)          (GVfsBackend *backend,
                                 GVfsJobOpenForWrite *job,
                                 const char *filename,
                                 GFileCreateFlags flags);
  void     (*edit)              (GVfsBackend *backend,
                                 GVfsJobOpenForWrite *job,
                                 const char *filename,
                                 GFileCreateFlags flags);
  void     (*close_write)       (GVfsBackend *backend,
				 GVfsJobCloseWrite *job,
				 GVfsBackendHandle handle);
  gboolean (*try_close_write)   (GVfsBackend *backend,
				 GVfsJobCloseWrite *job,
				 GVfsBackendHandle handle);
  void     (*write)             (GVfsBackend *backend,
				 GVfsJobWrite *job,
				 GVfsBackendHandle handle,
				 char *buffer,
				 gsize buffer_size);
  gboolean (*try_write)         (GVfsBackend *backend,
				 GVfsJobWrite *job,
				 GVfsBackendHandle handle,
				 char *buffer,
				 gsize buffer_size);
  void     (*seek_on_write)     (GVfsBackend *backend,
				 GVfsJobSeekWrite *job,
				 GVfsBackendHandle handle,
				 goffset    offset,
				 GSeekType  type);
  gboolean (*try_seek_on_write) (GVfsBackend *backend,
				 GVfsJobSeekWrite *job,
				 GVfsBackendHandle handle,
				 goffset    offset,
				 GSeekType  type);
  void     (*truncate)          (GVfsBackend *backend,
				 GVfsJobTruncate *job,
				 GVfsBackendHandle handle,
				 goffset size);
  gboolean (*try_truncate)      (GVfsBackend *backend,
				 GVfsJobTruncate *job,
				 GVfsBackendHandle handle,
				 goffset size);
  void     (*query_info)        (GVfsBackend *backend,
				 GVfsJobQueryInfo *job,
				 const char *filename,
				 GFileQueryInfoFlags flags,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  gboolean (*try_query_info)    (GVfsBackend *backend,
				 GVfsJobQueryInfo *job,
				 const char *filename,
				 GFileQueryInfoFlags flags,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  void     (*query_info_on_read)(GVfsBackend *backend,
				 GVfsJobQueryInfoRead *job,
				 GVfsBackendHandle handle,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  gboolean (*try_query_info_on_read)(GVfsBackend *backend,
				 GVfsJobQueryInfoRead *job,
				 GVfsBackendHandle handle,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  void     (*query_info_on_write)(GVfsBackend *backend,
				 GVfsJobQueryInfoWrite *job,
				 GVfsBackendHandle handle,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  gboolean (*try_query_info_on_write)(GVfsBackend *backend,
				 GVfsJobQueryInfoWrite *job,
				 GVfsBackendHandle handle,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  void     (*query_fs_info)     (GVfsBackend *backend,
				 GVfsJobQueryFsInfo *job,
				 const char *filename,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  gboolean (*try_query_fs_info) (GVfsBackend *backend,
				 GVfsJobQueryFsInfo *job,
				 const char *filename,
				 GFileInfo *info,
				 GFileAttributeMatcher *attribute_matcher);
  void     (*enumerate)         (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 GFileAttributeMatcher *attribute_matcher,
				 GFileQueryInfoFlags flags);
  gboolean (*try_enumerate)     (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 GFileAttributeMatcher *attribute_matcher,
				 GFileQueryInfoFlags flags);
  void     (*set_display_name)  (GVfsBackend *backend,
				 GVfsJobSetDisplayName *job,
				 const char *filename,
				 const char *display_name);
  gboolean (*try_set_display_name) (GVfsBackend *backend,
				    GVfsJobSetDisplayName *job,
				    const char *filename,
				    const char *display_name);
  void     (*delete)            (GVfsBackend *backend,
				 GVfsJobDelete *job,
				 const char *filename);
  gboolean (*try_delete)        (GVfsBackend *backend,
				 GVfsJobDelete *job,
				 const char *filename);
  void     (*trash)             (GVfsBackend *backend,
				 GVfsJobTrash *job,
				 const char *filename);
  gboolean (*try_trash)         (GVfsBackend *backend,
				 GVfsJobTrash *job,
				 const char *filename);
  void     (*make_directory)    (GVfsBackend *backend,
				 GVfsJobMakeDirectory *job,
				 const char *filename);
  gboolean (*try_make_directory)(GVfsBackend *backend,
				 GVfsJobMakeDirectory *job,
				 const char *filename);
  void     (*make_symlink)      (GVfsBackend *backend,
				 GVfsJobMakeSymlink *make_symlink,
				 const char *filename,
				 const char *symlink_value);
  gboolean (*try_make_symlink)  (GVfsBackend *backend,
				 GVfsJobMakeSymlink *make_symlink,
				 const char *filename,
				 const char *symlink_value);
  void     (*copy)              (GVfsBackend *backend,
				 GVfsJobCopy *job,
				 const char *source,
				 const char *destination,
				 GFileCopyFlags flags,
				 GFileProgressCallback progress_callback,
				 gpointer progress_callback_data);
  gboolean (*try_copy)          (GVfsBackend *backend,
				 GVfsJobCopy *job,
				 const char *source,
				 const char *destination,
				 GFileCopyFlags flags,
				 GFileProgressCallback progress_callback,
				 gpointer progress_callback_data);
   void     (*move)              (GVfsBackend *backend,
				 GVfsJobMove *job,
				 const char *source,
				 const char *destination,
				 GFileCopyFlags flags,
				 GFileProgressCallback progress_callback,
				 gpointer progress_callback_data);
  gboolean (*try_move)          (GVfsBackend *backend,
				 GVfsJobMove *job,
				 const char *source,
				 const char *destination,
				 GFileCopyFlags flags,
				 GFileProgressCallback progress_callback,
				 gpointer progress_callback_data);
  void     (*push)             (GVfsBackend *backend,
                                GVfsJobPush *job,
                                const char *destination,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data);
  gboolean (*try_push)         (GVfsBackend *backend,
                                GVfsJobPush *job,
                                const char *destination,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data);
  void     (*pull)             (GVfsBackend *backend,
                                GVfsJobPull *job,
                                const char *source,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data);
  gboolean (*try_pull)         (GVfsBackend *backend,
                                GVfsJobPull *job,
                                const char *source,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data);
  void     (*set_attribute)     (GVfsBackend *backend,
				 GVfsJobSetAttribute *set_attribute,
				 const char *filename,
				 const char *attribute,
				 GFileAttributeType type,
				 gpointer value_p,
				 GFileQueryInfoFlags flags);
  gboolean (*try_set_attribute) (GVfsBackend *backend,
				 GVfsJobSetAttribute *set_attribute,
				 const char *filename,
				 const char *attribute,
				 GFileAttributeType type,
				 gpointer value_p,
				 GFileQueryInfoFlags flags);
  void     (*create_dir_monitor)(GVfsBackend *backend,
				 GVfsJobCreateMonitor *job,
				 const char *filename,
				 GFileMonitorFlags flags);
  gboolean (*try_create_dir_monitor) (GVfsBackend *backend,
				      GVfsJobCreateMonitor *job,
				      const char *filename,
				      GFileMonitorFlags flags);
  void     (*create_file_monitor)(GVfsBackend *backend,
				  GVfsJobCreateMonitor *job,
				  const char *filename,
				  GFileMonitorFlags flags);
  gboolean (*try_create_file_monitor) (GVfsBackend *backend,
				       GVfsJobCreateMonitor *job,
				       const char *filename,
				       GFileMonitorFlags flags);
  void (*query_settable_attributes)         (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename);
  gboolean (*try_query_settable_attributes) (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename);
  void (*query_writable_namespaces)         (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename);
  gboolean (*try_query_writable_namespaces) (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename);

  void     (*start_mountable)   (GVfsBackend *backend,
				 GVfsJobStartMountable *job,
				 const char *filename,
				 GMountSource *mount_source);
  gboolean (*try_start_mountable) (GVfsBackend *backend,
                                   GVfsJobStartMountable *job,
                                   const char *filename,
                                   GMountSource *mount_source);
  void     (*stop_mountable) (GVfsBackend *backend,
                              GVfsJobStopMountable *job,
                              const char *filename,
                              GMountUnmountFlags flags,
                              GMountSource *mount_source);
  gboolean (*try_stop_mountable)   (GVfsBackend *backend,
				    GVfsJobStopMountable *job,
				    const char *filename,
				    GMountUnmountFlags flags,
                                    GMountSource *mount_source);
  void     (*poll_mountable) (GVfsBackend *backend,
                              GVfsJobPollMountable *job,
                              const char *filename);
  gboolean (*try_poll_mountable)   (GVfsBackend *backend,
				    GVfsJobPollMountable *job,
				    const char *filename);
};

GType g_vfs_backend_get_type (void) G_GNUC_CONST;

void  g_vfs_register_backend       (GType               backend_type,
				    const char         *type);
GType g_vfs_lookup_backend         (const char         *type);

void        g_vfs_backend_set_display_name               (GVfsBackend        *backend,
							  const char         *display_name);
void        g_vfs_backend_set_stable_name                (GVfsBackend        *backend,
							  const char         *stable_name);
void        g_vfs_backend_set_x_content_types            (GVfsBackend        *backend,
							  char              **x_content_types);
void        g_vfs_backend_set_icon_name                  (GVfsBackend        *backend,
							  const char         *icon_name);
void        g_vfs_backend_set_icon                       (GVfsBackend        *backend,
							  GIcon              *icon);
void        g_vfs_backend_set_symbolic_icon_name         (GVfsBackend        *backend,
							  const char         *icon_name);
void        g_vfs_backend_set_symbolic_icon              (GVfsBackend        *backend,
							  GIcon              *icon);
void        g_vfs_backend_set_prefered_filename_encoding (GVfsBackend        *backend,
							  const char         *prefered_filename_encoding);
void        g_vfs_backend_set_user_visible               (GVfsBackend        *backend,
							  gboolean            user_visible);
void        g_vfs_backend_set_default_location           (GVfsBackend        *backend,
							  const char         *location);
void        g_vfs_backend_set_mount_spec                 (GVfsBackend        *backend,
							  GMountSpec         *mount_spec);
void        g_vfs_backend_register_mount                 (GVfsBackend        *backend,
                                                          GAsyncReadyCallback callback,
							  gpointer            user_data);
gboolean    g_vfs_backend_register_mount_finish          (GVfsBackend        *backend,
                                                          GAsyncResult       *res,
                                                          GError            **error);
void        g_vfs_backend_unregister_mount               (GVfsBackend        *backend,
                                                          GAsyncReadyCallback callback,
							  gpointer            user_data);
gboolean    g_vfs_backend_unregister_mount_finish        (GVfsBackend        *backend,
                                                          GAsyncResult       *res,
                                                          GError            **error);
const char *g_vfs_backend_get_backend_type               (GVfsBackend        *backend);
const char *g_vfs_backend_get_display_name               (GVfsBackend        *backend);
const char *g_vfs_backend_get_stable_name                (GVfsBackend        *backend);
char      **g_vfs_backend_get_x_content_types            (GVfsBackend        *backend);
GIcon      *g_vfs_backend_get_icon                       (GVfsBackend        *backend);
GIcon      *g_vfs_backend_get_symbolic_icon              (GVfsBackend        *backend);
const char *g_vfs_backend_get_default_location           (GVfsBackend        *backend);
GMountSpec *g_vfs_backend_get_mount_spec                 (GVfsBackend        *backend);
GVfsDaemon *g_vfs_backend_get_daemon                     (GVfsBackend        *backend);
gboolean    g_vfs_backend_is_mounted                     (GVfsBackend        *backend);
void        g_vfs_backend_force_unmount                  (GVfsBackend        *backend);

void        g_vfs_backend_add_auto_info                  (GVfsBackend           *backend,
							  GFileAttributeMatcher *matcher,
							  GFileInfo             *info,
							  const char            *uri);
void        g_vfs_backend_add_auto_fs_info               (GVfsBackend           *backend,
                                                          GFileAttributeMatcher *matcher,
                                                          GFileInfo             *info);

void        g_vfs_backend_set_block_requests             (GVfsBackend           *backend,
                                                          gboolean               value);
gboolean    g_vfs_backend_get_block_requests             (GVfsBackend           *backend);

gboolean    g_vfs_backend_unmount_with_operation_finish (GVfsBackend  *backend,
                                                         GAsyncResult *res,
                                                         GError      **error);

void        g_vfs_backend_unmount_with_operation (GVfsBackend        *backend,
                                                  GMountSource       *mount_source,
                                                  GAsyncReadyCallback callback,
                                                  gpointer            user_data);

gboolean    g_vfs_backend_invocation_first_handler       (GVfsDBusMount *object,
                                                          GDBusMethodInvocation *invocation,
                                                          GVfsBackend *backend);

void        g_vfs_backend_handle_readonly_lockdown       (GVfsBackend *backend);
gboolean    g_vfs_backend_get_readonly_lockdown          (GVfsBackend *backend);

G_END_DECLS

#endif /* __G_VFS_BACKEND_H__ */
