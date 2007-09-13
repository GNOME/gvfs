#ifndef __G_VFS_BACKEND_H__
#define __G_VFS_BACKEND_H__

#include <dbus/dbus.h>
#include <gio/gvfstypes.h>
#include <gio/gfileinfo.h>
#include <gvfsdaemon.h>
#include <gvfsjob.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_BACKEND         (g_vfs_backend_get_type ())
#define G_VFS_BACKEND(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_BACKEND, GVfsBackend))
#define G_VFS_BACKEND_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_BACKEND, GVfsBackendClass))
#define G_IS_VFS_BACKEND(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_BACKEND))
#define G_IS_VFS_BACKEND_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_BACKEND))
#define G_VFS_BACKEND_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_BACKEND, GVfsBackendClass))

typedef struct _GVfsBackend        GVfsBackend;
typedef struct _GVfsBackendClass   GVfsBackendClass;

typedef struct _GVfsJobOpenForRead  GVfsJobOpenForRead;
typedef struct _GVfsJobSeekRead     GVfsJobSeekRead;
typedef struct _GVfsJobCloseRead    GVfsJobCloseRead;
typedef struct _GVfsJobRead         GVfsJobRead;
typedef struct _GVfsJobGetInfo      GVfsJobGetInfo;
typedef struct _GVfsJobEnumerate    GVfsJobEnumerate;

typedef gpointer GVfsBackendHandle;

struct _GVfsBackend
{
  GObject parent_instance;
  char *object_path;
  char *display_name;
  GMountSpec *mount_spec;
};

struct _GVfsBackendClass
{
  GObjectClass parent_class;

  /* vtable */

  /* These should all be fast and non-blocking, scheduling the i/o
   * operations async (or on a thread).
   * Returning FALSE means "Can't do this right now, try later" 
   * Returning TRUE means you started the job and will set the
   * result (or error) on the opernation object when done.
   * A NULL here means operation not supported 
   */

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

void     g_vfs_backend_register_with_daemon (GVfsBackend           *backend,
					     GVfsDaemon            *daemon);


G_END_DECLS

#endif /* __G_VFS_BACKEND_H__ */
