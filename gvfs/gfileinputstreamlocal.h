#ifndef __G_FILE_INPUT_STREAM_LOCAL_H__
#define __G_FILE_INPUT_STREAM_LOCAL_H__

#include <gvfs/gfileinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_INPUT_STREAM_LOCAL         (g_file_input_stream_local_get_type ())
#define G_FILE_INPUT_STREAM_LOCAL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_INPUT_STREAM_LOCAL, GFileInputStreamLocal))
#define G_FILE_INPUT_STREAM_LOCAL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_INPUT_STREAM_LOCAL, GFileInputStreamLocalClass))
#define G_IS_FILE_INPUT_STREAM_LOCAL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_INPUT_STREAM_LOCAL))
#define G_IS_FILE_INPUT_STREAM_LOCAL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_INPUT_STREAM_LOCAL))
#define G_FILE_INPUT_STREAM_LOCAL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_INPUT_STREAM_LOCAL, GFileInputStreamLocalClass))

typedef struct _GFileInputStreamLocal         GFileInputStreamLocal;
typedef struct _GFileInputStreamLocalClass    GFileInputStreamLocalClass;
typedef struct _GFileInputStreamLocalPrivate  GFileInputStreamLocalPrivate;

struct _GFileInputStreamLocal
{
  GFileInputStream parent;

  /*< private >*/
  GFileInputStreamLocalPrivate *priv;
};

struct _GFileInputStreamLocalClass
{
  GFileInputStreamClass parent_class;
};

GType g_file_input_stream_local_get_type (void) G_GNUC_CONST;

GFileInputStream *g_file_input_stream_local_new (int fd);

G_END_DECLS

#endif /* __G_FILE_INPUT_STREAM_LOCAL_H__ */
