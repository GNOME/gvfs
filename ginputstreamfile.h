#ifndef __G_INPUT_STREAM_FILE_H__
#define __G_INPUT_STREAM_FILE_H__

#include "ginputstream.h"

G_BEGIN_DECLS

#define G_TYPE_INPUT_STREAM_FILE         (g_input_stream_file_get_type ())
#define G_INPUT_STREAM_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_INPUT_STREAM_FILE, GInputStreamFile))
#define G_INPUT_STREAM_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_INPUT_STREAM_FILE, GInputStreamFileClass))
#define G_IS_INPUT_STREAM_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_INPUT_STREAM_FILE))
#define G_IS_INPUT_STREAM_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_INPUT_STREAM_FILE))
#define G_INPUT_STREAM_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_INPUT_STREAM_FILE, GInputStreamFileClass))

typedef struct _GInputStreamFile         GInputStreamFile;
typedef struct _GInputStreamFileClass    GInputStreamFileClass;
typedef struct _GInputStreamFilePrivate  GInputStreamFilePrivate;

struct _GInputStreamFile
{
  GInputStream parent;

  /*< private >*/
  GInputStreamFilePrivate *priv;
};

struct _GInputStreamFileClass
{
  GInputStreamClass parent_class;
};

GType g_input_stream_file_get_type (void) G_GNUC_CONST;

GInputStream *g_input_stream_file_new    (const char *filename);
int           g_input_stream_file_get_fd (GInputStream *stream);

G_END_DECLS

#endif /* __G_INPUT_STREAM_FILE_H__ */
