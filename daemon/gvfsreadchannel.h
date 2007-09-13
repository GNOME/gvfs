#ifndef __G_VFS_READ_CHANNEL_H__
#define __G_VFS_READ_CHANNEL_H__

#include <glib-object.h>
#include <gvfsjob.h>
#include <gvfschannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_READ_CHANNEL         (g_vfs_read_channel_get_type ())
#define G_VFS_READ_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannel))
#define G_VFS_READ_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannelClass))
#define G_VFS_IS_READ_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_READ_CHANNEL))
#define G_VFS_IS_READ_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_READ_CHANNEL))
#define G_VFS_READ_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannelClass))

typedef struct _GVfsReadChannel        GVfsReadChannel;
typedef struct _GVfsReadChannelClass   GVfsReadChannelClass;
typedef struct _GVfsReadChannelPrivate GVfsReadChannelPrivate;

struct _GVfsReadChannelClass
{
  GVfsChannelClass parent_class;
};

GType g_vfs_read_channel_get_type (void) G_GNUC_CONST;

GVfsReadChannel *g_vfs_read_channel_new                (GVfsBackend        *backend);
void            g_vfs_read_channel_send_data          (GVfsReadChannel     *read_channel,
						       char               *buffer,
						       gsize               count);
void            g_vfs_read_channel_send_closed        (GVfsReadChannel     *read_channel);
void            g_vfs_read_channel_send_seek_offset   (GVfsReadChannel     *read_channel,
						      goffset             offset);

G_END_DECLS

#endif /* __G_VFS_READ_CHANNEL_H__ */
