#ifndef __G_INPUT_STREAM_SOCKET_H__
#define __G_INPUT_STREAM_SOCKET_H__

#include <gio/ginputstream.h>

G_BEGIN_DECLS

#define G_TYPE_INPUT_STREAM_SOCKET         (g_input_stream_socket_get_type ())
#define G_INPUT_STREAM_SOCKET(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_INPUT_STREAM_SOCKET, GInputStreamSocket))
#define G_INPUT_STREAM_SOCKET_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_INPUT_STREAM_SOCKET, GInputStreamSocketClass))
#define G_IS_INPUT_STREAM_SOCKET(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_INPUT_STREAM_SOCKET))
#define G_IS_INPUT_STREAM_SOCKET_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_INPUT_STREAM_SOCKET))
#define G_INPUT_STREAM_SOCKET_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_INPUT_STREAM_SOCKET, GInputStreamSocketClass))

typedef struct _GInputStreamSocket         GInputStreamSocket;
typedef struct _GInputStreamSocketClass    GInputStreamSocketClass;
typedef struct _GInputStreamSocketPrivate  GInputStreamSocketPrivate;

struct _GInputStreamSocket
{
  GInputStream parent;

  /*< private >*/
  GInputStreamSocketPrivate *priv;
};

struct _GInputStreamSocketClass
{
  GInputStreamClass parent_class;
};

GType g_input_stream_socket_get_type (void) G_GNUC_CONST;

GInputStream *g_input_stream_socket_new (int fd,
					 gboolean close_fd_at_close);

G_END_DECLS

#endif /* __G_INPUT_STREAM_SOCKET_H__ */
