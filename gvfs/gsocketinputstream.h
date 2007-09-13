#ifndef __G_SOCKET_INPUT_STREAM_H__
#define __G_SOCKET_INPUT_STREAM_H__

#include <gvfs/ginputstream.h>

G_BEGIN_DECLS

#define G_TYPE_SOCKET_INPUT_STREAM         (g_socket_input_stream_get_type ())
#define G_SOCKET_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_SOCKET_INPUT_STREAM, GSocketInputStream))
#define G_SOCKET_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_SOCKET_INPUT_STREAM, GSocketInputStreamClass))
#define G_IS_SOCKET_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_SOCKET_INPUT_STREAM))
#define G_IS_SOCKET_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_SOCKET_INPUT_STREAM))
#define G_SOCKET_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_SOCKET_INPUT_STREAM, GSocketInputStreamClass))

typedef struct _GSocketInputStream         GSocketInputStream;
typedef struct _GSocketInputStreamClass    GSocketInputStreamClass;
typedef struct _GSocketInputStreamPrivate  GSocketInputStreamPrivate;

struct _GSocketInputStream
{
  GInputStream parent;

  /*< private >*/
  GSocketInputStreamPrivate *priv;
};

struct _GSocketInputStreamClass
{
  GInputStreamClass parent_class;
};

GType g_socket_input_stream_get_type (void) G_GNUC_CONST;

GInputStream *g_socket_input_stream_new (int fd,
					 gboolean close_fd_at_close);

G_END_DECLS

#endif /* __G_SOCKET_INPUT_STREAM_H__ */
