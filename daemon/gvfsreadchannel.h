#ifndef __G_VFS_READ_CHANNEL_H__
#define __G_VFS_READ_CHANNEL_H__

#include <glib-object.h>
#include <gio/gvfstypes.h>
#include <gvfsjob.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_READ_CHANNEL         (g_vfs_read_channel_get_type ())
#define G_VFS_READ_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_READ_CHANNEL, GVfsReadChannel))
#define G_VFS_READ_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_READ_CHANNEL, GVfsReadChannelClass))
#define G_IS_VFS_READ_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_READ_CHANNEL))
#define G_IS_VFS_READ_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_READ_CHANNEL))
#define G_VFS_READ_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_READ_CHANNEL, GVfsReadChannelClass))

typedef struct _GVfsReadChannel        GVfsReadChannel;
typedef struct _GVfsReadChannelClass   GVfsReadChannelClass;
typedef struct _GVfsReadChannelPrivate GVfsReadChannelPrivate;

struct _GVfsReadChannel
{
  GObject parent_instance;

  GVfsReadChannelPrivate *priv;
};

struct _GVfsReadChannelClass
{
  GObjectClass parent_class;
};

GType g_vfs_read_channel_get_type (void) G_GNUC_CONST;

GVfsReadChannel *g_vfs_read_channel_new                (GVfsBackend        *backend,
							GError            **error);
int             g_vfs_read_channel_steal_remote_fd    (GVfsReadChannel     *read_channel);
GVfsBackend    *g_vfs_read_channel_get_backend        (GVfsReadChannel     *read_channel);
void            g_vfs_read_channel_set_backend_handle (GVfsReadChannel     *read_channel,
						       GVfsBackendHandle   backend_handle);
gboolean        g_vfs_read_channel_has_job            (GVfsReadChannel     *read_channel);
GVfsJob *       g_vfs_read_channel_get_job            (GVfsReadChannel     *read_channel);
void            g_vfs_read_channel_send_data          (GVfsReadChannel     *read_channel,
						       char               *buffer,
						       gsize               count);
void            g_vfs_read_channel_send_error         (GVfsReadChannel     *read_channel,
						       GError             *error);
void            g_vfs_read_channel_send_closed        (GVfsReadChannel     *read_channel);
void            g_vfs_read_channel_send_seek_offset   (GVfsReadChannel     *read_channel,
						      goffset             offset);

/* TODO: i/o priority? */

G_END_DECLS

#endif /* __G_VFS_READ_CHANNEL_H__ */
