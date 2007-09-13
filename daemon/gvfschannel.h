#ifndef __G_VFS_CHANNEL_H__
#define __G_VFS_CHANNEL_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfsdaemonprotocol.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_CHANNEL         (g_vfs_channel_get_type ())
#define G_VFS_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_CHANNEL, GVfsChannel))
#define G_VFS_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_CHANNEL, GVfsChannelClass))
#define G_VFS_IS_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_CHANNEL))
#define G_VFS_IS_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_CHANNEL))
#define G_VFS_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_CHANNEL, GVfsChannelClass))

typedef struct _GVfsChannel        GVfsChannel;
typedef struct _GVfsChannelClass   GVfsChannelClass;
typedef struct _GVfsChannelPrivate GVfsChannelPrivate;

struct _GVfsChannel
{
  GObject parent_instance;

  GVfsChannelPrivate *priv;
};

struct _GVfsChannelClass
{
  GObjectClass parent_class;

  GVfsJob *(*close)          (GVfsChannel *channel);
  GVfsJob *(*handle_request) (GVfsChannel *channel,
			      guint32 command,
			      guint32 seq_nr,
			      guint32 arg1,
			      guint32 arg2,
			      gpointer data,
			      gsize data_len,
			      GError **error);
};

GType g_vfs_channel_get_type (void) G_GNUC_CONST;

int               g_vfs_channel_steal_remote_fd    (GVfsChannel                   *channel);
GVfsBackend    *  g_vfs_channel_get_backend        (GVfsChannel                   *channel);
GVfsBackendHandle g_vfs_channel_get_backend_handle (GVfsChannel                   *channel);
void              g_vfs_channel_set_backend_handle (GVfsChannel                   *channel,
						    GVfsBackendHandle              backend_handle);
gboolean          g_vfs_channel_has_job            (GVfsChannel                   *channel);
GVfsJob *         g_vfs_channel_get_job            (GVfsChannel                   *channel);
void              g_vfs_channel_send_error         (GVfsChannel                   *channel,
						    GError                        *error);
void              g_vfs_channel_send_reply         (GVfsChannel                   *channel,
						    GVfsDaemonSocketProtocolReply *reply,
						    const void                    *data,
						    gsize                          data_len);
guint32           g_vfs_channel_get_current_seq_nr (GVfsChannel                   *channel);

/* TODO: i/o priority? */

G_END_DECLS

#endif /* __G_VFS_CHANNEL_H__ */
