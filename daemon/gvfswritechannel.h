#ifndef __G_VFS_WRITE_CHANNEL_H__
#define __G_VFS_WRITE_CHANNEL_H__

#include <glib-object.h>
#include <gvfsjob.h>
#include <gvfschannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_WRITE_CHANNEL         (g_vfs_write_channel_get_type ())
#define G_VFS_WRITE_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannel))
#define G_VFS_WRITE_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannelClass))
#define G_VFS_IS_WRITE_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_WRITE_CHANNEL))
#define G_VFS_IS_WRITE_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_WRITE_CHANNEL))
#define G_VFS_WRITE_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannelClass))

typedef struct _GVfsWriteChannel        GVfsWriteChannel;
typedef struct _GVfsWriteChannelClass   GVfsWriteChannelClass;
typedef struct _GVfsWriteChannelPrivate GVfsWriteChannelPrivate;

struct _GVfsWriteChannelClass
{
  GVfsChannelClass parent_class;
};

GType g_vfs_write_channel_get_type (void) G_GNUC_CONST;

GVfsWriteChannel *g_vfs_write_channel_new              (GVfsBackend      *backend);
void              g_vfs_write_channel_send_written     (GVfsWriteChannel *write_channel,
							gsize             bytes_written);
void              g_vfs_write_channel_send_closed      (GVfsWriteChannel *write_channel);
void              g_vfs_write_channel_send_seek_offset (GVfsWriteChannel *write_channel,
							goffset           offset);

G_END_DECLS

#endif /* __G_VFS_WRITE_CHANNEL_H__ */
