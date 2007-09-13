#ifndef __G_VFS_BACKEND_H__
#define __G_VFS_BACKEND_H__

#include <dbus/dbus.h>
#include <gio/gvfstypes.h>
#include <gio/gfileinfo.h>
#include <gvfsdaemon.h>
#include <gvfsjob.h>
#include <gmountspec.h>
#include <gdbusutils.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND         (g_vfs_backend_get_type ())
#define G_VFS_BACKEND(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND, GVfsBackend))
#define G_VFS_BACKEND_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND, GVfsBackendClass))
#define G_IS_VFS_BACKEND(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND))
#define G_IS_VFS_BACKEND_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND))
#define G_VFS_BACKEND_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND, GVfsBackendClass))

typedef struct _GVfsBackend        GVfsBackend;
typedef struct _GVfsBackendPrivate GVfsBackendPrivate;
typedef struct _GVfsBackendClass   GVfsBackendClass;

typedef struct _GVfsJobMount        GVfsJobMount;
typedef struct _GVfsJobOpenForRead  GVfsJobOpenForRead;
typedef struct _GVfsJobSeekRead     GVfsJobSeekRead;
typedef struct _GVfsJobCloseRead    GVfsJobCloseRead;
typedef struct _GVfsJobRead         GVfsJobRead;
typedef struct _GVfsJobOpenForWrite GVfsJobOpenForWrite;
typedef struct _GVfsJobWrite        GVfsJobWrite;
typedef struct _GVfsJobSeekWrite    GVfsJobSeekWrite;
typedef struct _GVfsJobCloseWrite   GVfsJobCloseWrite;
typedef struct _GVfsJobGetInfo      GVfsJobGetInfo;
typedef struct _GVfsJobEnumerate    GVfsJobEnumerate;

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
				 GMountSource *mount_source);
  gboolean (*try_mount)         (GVfsBackend *backend,
				 GVfsJobMount *job,
				 GMountSpec *mount_spec,
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
				 GFileInfoRequestFlags requested,
				 const char *attributes,
				 gboolean follow_symlinks);
  gboolean (*try_get_info)      (GVfsBackend *backend,
				 GVfsJobGetInfo *job,
				 const char *filename,
				 GFileInfoRequestFlags requested,
				 const char *attributes,
				 gboolean follow_symlinks);
  void     (*enumerate)         (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 GFileInfoRequestFlags requested,
				 const char *attributes,
				 gboolean follow_symlinks);
  gboolean (*try_enumerate)     (GVfsBackend *backend,
				 GVfsJobEnumerate *job,
				 const char *filename,
				 GFileInfoRequestFlags requested,
				 const char *attributes,
				 gboolean follow_symlinks);
};

GType g_vfs_backend_get_type (void) G_GNUC_CONST;

void  g_vfs_register_backend       (GType               backend_type,
				    const char         *type);
GType g_vfs_lookup_backend         (const char         *type);

void g_vfs_backend_set_display_name (GVfsBackend        *backend,
				     const char         *display_name);
void g_vfs_backend_set_icon         (GVfsBackend        *backend,
				     const char         *icon);
void g_vfs_backend_set_mount_spec   (GVfsBackend        *backend,
				     GMountSpec         *mount_spec);
void g_vfs_backend_register_mount   (GVfsBackend        *backend,
				     GAsyncDBusCallback  callback,
				     gpointer            user_data);



G_END_DECLS

#endif /* __G_VFS_BACKEND_H__ */
