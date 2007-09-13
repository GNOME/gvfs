#ifndef __G_OUTPUT_STREAM_SOCKET_H__
#define __G_OUTPUT_STREAM_SOCKET_H__

#include <gio/goutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_OUTPUT_STREAM_SOCKET         (g_output_stream_socket_get_type ())
#define G_OUTPUT_STREAM_SOCKET(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_OUTPUT_STREAM_SOCKET, GOutputStreamSocket))
#define G_OUTPUT_STREAM_SOCKET_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_OUTPUT_STREAM_SOCKET, GOutputStreamSocketClass))
#define G_IS_OUTPUT_STREAM_SOCKET(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_OUTPUT_STREAM_SOCKET))
#define G_IS_OUTPUT_STREAM_SOCKET_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_OUTPUT_STREAM_SOCKET))
#define G_OUTPUT_STREAM_SOCKET_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_OUTPUT_STREAM_SOCKET, GOutputStreamSocketClass))

typedef struct _GOutputStreamSocket         GOutputStreamSocket;
typedef struct _GOutputStreamSocketClass    GOutputStreamSocketClass;
typedef struct _GOutputStreamSocketPrivate  GOutputStreamSocketPrivate;

struct _GOutputStreamSocket
{
  GOutputStream parent;

  /*< private >*/
  GOutputStreamSocketPrivate *priv;
};

struct _GOutputStreamSocketClass
{
  GOutputStreamClass parent_class;
};

GType g_output_stream_socket_get_type (void) G_GNUC_CONST;

GOutputStream *g_output_stream_socket_new (int fd,
					   gboolean close_fd_at_close);

G_END_DECLS

#endif /* __G_OUTPUT_STREAM_SOCKET_H__ */
