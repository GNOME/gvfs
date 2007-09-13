#ifndef __G_VFS_BACKEND_H__
#define __G_VFS_BACKEND_H__

#include <dbus/dbus.h>
#include <gio/giotypes.h>
#include <gio/gfileinfo.h>
#include <gio/gfile.h>
#include <gvfsdaemon.h>
#include <gvfsjob.h>
#include <gmountspec.h>
#include <gdbusutils.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND         (g_vfs_backend_get_type ())
#define G_VFS_BACKEND(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND, GVfsBackend))
#define G_VFS_BACKEND_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND, GVfsBackendClass))
#define G_VFS_IS_BACKEND(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND))
#define G_VFS_IS_BACKEND_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND))
#define G_VFS_BACKEND_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND, GVfsBackendClass))

typedef struct _GVfsBackend        GVfsBackend;
typedef struct _GVfsBackendPrivate GVfsBackendPrivate;
typedef struct _GVfsBackendClass   GVfsBackendClass;

typedef struct _GVfsJobMount           GVfsJobMount;
typedef struct _GVfsJobMountMountable  GVfsJobMountMountable;
typedef struct _GVfsJobOpenForRead     GVfsJobOpenForRead;
typedef struct _GVfsJobSeekRead        GVfsJobSeekRead;
typedef struct _GVfsJobCloseRead       GVfsJobCloseRead;
typedef struct _GVfsJobRead            GVfsJobRead;
typedef struct _GVfsJobOpenForWrite    GVfsJobOpenForWrite;
typedef struct _GVfsJobWrite           GVfsJobWrite;
typedef struct _GVfsJobSeekWrite       GVfsJobSeekWrite;
typedef struct _GVfsJobCloseWrite      GVfsJobCloseWrite;
typedef struct _GVfsJobGetInfo         GVfsJobGetInfo;
typedef struct _GVfsJobGetFsInfo       GVfsJobGetFsInfo;
typedef struct _GVfsJobEnumerate       GVfsJobEnumerate;
typedef struct _GVfsJobSetDisplayName  GVfsJobSetDisplayName;
typedef struct _GVfsJobTrash           GVfsJobTrash;
typedef struct _GVfsJobDelete          GVfsJobDelete;
typedef struct _GVfsJobMakeDirectory   GVfsJobMakeDirectory;
typedef struct _GVfsJobMakeSymlink     GVfsJobMakeSymlink;
typedef struct _GVfsJobCopy            GVfsJobCopy;
typedef struct _GVfsJobMove            GVfsJobMove;
typedef struct _GVfsJobSetAttribute    GVfsJobSetAttribute;
typedef struct _GVfsJobQueryAttributes GVfsJobQueryAttributes;

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
  void     (*open_for_read)     (GVfsBackend *backend,
				 GVfsJobOpenForRead *job,
				 const char *filename);
  gboolean (*try_open_for_read) (GVfsBackend *backend,
				 GVfsJobOpenForRead *job,
				 const char *filename);
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
				 const char *filename);
  void     (*create)            (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename);
  gboolean (*try_append_to)     (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename);
  void     (*append_to)         (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename);
  gboolean (*try_replace)       (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 time_t mtime,
				 gboolean make_backup);
  void     (*replace)           (GVfsBackend *backend,
				 GVfsJobOpenForWrite *job,
				 const char *filename,
				 time_t mtime,
				 gboolean make_backup);
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
  void     (*get_info)          (GVfsBackend *backend,
				 GVfsJobGetInfo *job,
				 const char *filename,
				 const char *attributes,
				 GFileGetInfoFlags flags);
  gboolean (*try_get_info)      (GVfsBackend *backend,
				 GVfsJobGetInfo *job,
				 const char *filename,
				 const char *attributes,
				 GFileGetInfoFlags flags);
  void     (*get_fs_info)       (GVfsBackend *backend,
				 GVfsJobGetFsInfo *job,
				 const char *filename,
				 const char *attributes);
  gboolean (*try_get_fs_info)   (GVfsBackend *backend,
				 GVfsJobGetFsInfo *job,
				 const char *filename,
				 const char *attributes);
  void     (*enumerate)         (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 const char *attributes,
				 GFileGetInfoFlags flags);
  gboolean (*try_enumerate)     (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 const char *attributes,
				 GFileGetInfoFlags flags);
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
				 GVfsJobMakeSymlink *make_directory,
				 const char *filename,
				 const char *symlink_value);
  gboolean (*try_make_symlink)  (GVfsBackend *backend,
				 GVfsJobMakeSymlink *make_directory,
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
  void     (*set_attribute)     (GVfsBackend *backend,
				 GVfsJobSetAttribute *set_attribute,
				 const char *filename,
				 const char *attribute,
				 GFileAttributeValue *value,
				 GFileGetInfoFlags flags);
  gboolean (*try_set_attribute) (GVfsBackend *backend,
				 GVfsJobSetAttribute *set_attribute,
				 const char *filename,
				 const char *attribute,
				 GFileAttributeValue *value,
				 GFileGetInfoFlags flags);
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
};

GType g_vfs_backend_get_type (void) G_GNUC_CONST;

void  g_vfs_register_backend       (GType               backend_type,
				    const char         *type);
GType g_vfs_lookup_backend         (const char         *type);

void        g_vfs_backend_set_display_name (GVfsBackend        *backend,
					    const char         *display_name);
void        g_vfs_backend_set_icon         (GVfsBackend        *backend,
					    const char         *icon);
void        g_vfs_backend_set_mount_spec   (GVfsBackend        *backend,
					    GMountSpec         *mount_spec);
void        g_vfs_backend_register_mount   (GVfsBackend        *backend,
					    GAsyncDBusCallback  callback,
					    gpointer            user_data);
const char *g_vfs_backend_get_backend_type (GVfsBackend        *backend);




G_END_DECLS

#endif /* __G_VFS_BACKEND_H__ */
