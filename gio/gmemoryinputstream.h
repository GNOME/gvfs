#ifndef __G_MEMORY_INPUT_STREAM_H__
#define __G_MEMORY_INPUT_STREAM_H__

#include <glib-object.h>
#include <gio/ginputstream.h>

G_BEGIN_DECLS

#define G_TYPE_MEMORY_INPUT_STREAM         (g_memory_input_stream_get_type ())
#define G_MEMORY_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MEMORY_INPUT_STREAM, GMemoryInputStream))
#define G_MEMORY_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MEMORY_INPUT_STREAM, GMemoryInputStreamClass))
#define G_IS_MEMORY_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MEMORY_INPUT_STREAM))
#define G_IS_MEMORY_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MEMORY_INPUT_STREAM))
#define G_MEMORY_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MEMORY_INPUT_STREAM, GMemoryInputStreamClass))

typedef struct _GMemoryInputStream         GMemoryInputStream;
typedef struct _GMemoryInputStreamClass    GMemoryInputStreamClass;
typedef struct _GMemoryInputStreamPrivate  GMemoryInputStreamPrivate;

struct _GMemoryInputStream
{
  GInputStream parent;

  /*< private >*/
  GMemoryInputStreamPrivate *priv;
};

struct _GMemoryInputStreamClass
{
 GInputStreamClass parent_class;
};


GType          g_memory_input_stream_get_type  (void) G_GNUC_CONST;
GInputStream * g_memory_input_stream_from_data (const void *data, gsize len);


G_END_DECLS

#endif /* __G_MEMORY_INPUT_STREAM_H__ */
