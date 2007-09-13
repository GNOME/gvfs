#ifndef __G_SOCKET_OUTPUT_STREAM_H__
#define __G_SOCKET_OUTPUT_STREAM_H__

#include <gvfs/goutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_SOCKET_OUTPUT_STREAM         (g_socket_output_stream_get_type ())
#define G_SOCKET_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_SOCKET_OUTPUT_STREAM, GSocketOutputStream))
#define G_SOCKET_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_SOCKET_OUTPUT_STREAM, GSocketOutputStreamClass))
#define G_IS_SOCKET_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_SOCKET_OUTPUT_STREAM))
#define G_IS_SOCKET_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_SOCKET_OUTPUT_STREAM))
#define G_SOCKET_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_SOCKET_OUTPUT_STREAM, GSocketOutputStreamClass))

typedef struct _GSocketOutputStream         GSocketOutputStream;
typedef struct _GSocketOutputStreamClass    GSocketOutputStreamClass;
typedef struct _GSocketOutputStreamPrivate  GSocketOutputStreamPrivate;

struct _GSocketOutputStream
{
  GOutputStream parent;

  /*< private >*/
  GSocketOutputStreamPrivate *priv;
};

struct _GSocketOutputStreamClass
{
  GOutputStreamClass parent_class;
};

GType g_socket_output_stream_get_type (void) G_GNUC_CONST;

GOutputStream *g_socket_output_stream_new (int fd,
					 gboolean close_fd_at_close);

G_END_DECLS

#endif /* __G_SOCKET_OUTPUT_STREAM_H__ */
