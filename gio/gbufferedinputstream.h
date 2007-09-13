#ifndef __G_BUFFERED_INPUT_STREAM_H__
#define __G_BUFFERED_INPUT_STREAM_H__

#include <glib-object.h>
#include <gio/gfilterinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_BUFFERED_INPUT_STREAM         (g_buffered_input_stream_get_type ())
#define G_BUFFERED_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_BUFFERED_INPUT_STREAM, GBufferedInputStream))
#define G_BUFFERED_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_BUFFERED_INPUT_STREAM, GBufferedInputStreamClass))
#define G_IS_BUFFERED_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_BUFFERED_INPUT_STREAM))
#define G_IS_BUFFERED_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_BUFFERED_INPUT_STREAM))
#define G_BUFFERED_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_BUFFERED_INPUT_STREAM, GBufferedInputStreamClass))

typedef struct _GBufferedInputStream         GBufferedInputStream;
typedef struct _GBufferedInputStreamClass    GBufferedInputStreamClass;
typedef struct _GBufferedInputStreamPrivate  GBufferedInputStreamPrivate;

struct _GBufferedInputStream
{
  GFilterInputStream parent;

  /*< private >*/
  GBufferedInputStreamPrivate *priv;
};

struct _GBufferedInputStreamClass
{
 GInputStreamClass parent_class;
};


GType          g_buffered_input_stream_get_type   (void) G_GNUC_CONST;
GInputStream*  g_buffered_input_stream_new        (GInputStream *base_stream);
GInputStream*  g_buffered_input_stream_new_sized  (GInputStream *base_stream,
                                                   guint         size);
G_END_DECLS

#endif /* __G_BUFFERED_INPUT_STREAM_H__ */
