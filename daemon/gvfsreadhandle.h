#ifndef __G_VFS_READ_HANDLE_H__
#define __G_VFS_READ_HANDLE_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_READ_HANDLE         (g_vfs_read_handle_get_type ())
#define G_VFS_READ_HANDLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_READ_HANDLE, GVfsReadHandle))
#define G_VFS_READ_HANDLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_READ_HANDLE, GVfsReadHandleClass))
#define G_IS_VFS_READ_HANDLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_READ_HANDLE))
#define G_IS_VFS_READ_HANDLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_READ_HANDLE))
#define G_VFS_READ_HANDLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_READ_HANDLE, GVfsReadHandleClass))

typedef struct _GVfsReadHandle        GVfsReadHandle;
typedef struct _GVfsReadHandleClass   GVfsReadHandleClass;
typedef struct _GVfsReadHandlePrivate GVfsReadHandlePrivate;

struct _GVfsReadHandle
{
  GObject parent_instance;

  GVfsReadHandlePrivate *priv;
};

struct _GVfsReadHandleClass
{
  GObjectClass parent_class;

  /* vtable */

  void (*wants_data) (GVfsReadHandle *handle);
};

GType g_vfs_read_handle_get_type (void) G_GNUC_CONST;

GVfsReadHandle *g_vfs_read_handle_new (GError **error);
int g_vfs_read_handle_get_fd (GVfsReadHandle *read_handle);
int g_vfs_read_handle_get_remote_fd (GVfsReadHandle *read_handle);
void g_vfs_read_handle_close_remote_fd (GVfsReadHandle *read_handle);
void g_vfs_read_handle_set_data (GVfsReadHandle *read_handle,
				 gpointer data);

gboolean g_vfs_read_handle_wants_data (GVfsReadHandle *read_handle);
gsize g_vfs_read_handle_get_requested_size (GVfsReadHandle *read_handle);
void g_vfs_read_handle_send_data (GVfsReadHandle *read_handle, 
				  char *data,
				  gsize length);

/* TODO: i/o priority? */

G_END_DECLS

#endif /* __G_VFS_READ_HANDLE_H__ */
