#ifndef __G_VFS_READ_STREAM_H__
#define __G_VFS_READ_STREAM_H__

#include <glib-object.h>
#include <gvfsjob.h>
#include <gvfs/gvfstypes.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_READ_STREAM         (g_vfs_read_stream_get_type ())
#define G_VFS_READ_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_READ_STREAM, GVfsReadStream))
#define G_VFS_READ_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_READ_STREAM, GVfsReadStreamClass))
#define G_IS_VFS_READ_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_READ_STREAM))
#define G_IS_VFS_READ_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_READ_STREAM))
#define G_VFS_READ_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_READ_STREAM, GVfsReadStreamClass))

typedef struct _GVfsReadStream        GVfsReadStream;
typedef struct _GVfsReadStreamClass   GVfsReadStreamClass;
typedef struct _GVfsReadStreamPrivate GVfsReadStreamPrivate;

struct _GVfsReadStream
{
  GObject parent_instance;

  GVfsReadStreamPrivate *priv;
};

struct _GVfsReadStreamClass
{
  GObjectClass parent_class;

  /* signals */

  void (*new_job) (GVfsReadStream *stream,
		   GVfsJob *job);
  void (*closed)  (GVfsReadStream *stream);
};

GType g_vfs_read_stream_get_type (void) G_GNUC_CONST;

GVfsReadStream *g_vfs_read_stream_new              (GError         **error);
int             g_vfs_read_stream_steal_remote_fd  (GVfsReadStream  *read_stream);
void            g_vfs_read_stream_set_user_data    (GVfsReadStream  *read_stream,
						    gpointer         data);
gboolean        g_vfs_read_stream_has_job          (GVfsReadStream  *read_stream);
GVfsJob *       g_vfs_read_stream_get_job          (GVfsReadStream  *read_stream);
void            g_vfs_read_stream_send_data        (GVfsReadStream  *read_stream,
						    char            *buffer,
						    gsize            count);
void            g_vfs_read_stream_send_error       (GVfsReadStream  *read_stream,
						    GError          *error);
void            g_vfs_read_stream_send_seek_offset (GVfsReadStream  *read_stream,
						    goffset          offset);

/* TODO: i/o priority? */

G_END_DECLS

#endif /* __G_VFS_READ_STREAM_H__ */
