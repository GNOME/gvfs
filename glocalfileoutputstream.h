#ifndef __G_LOCAL_FILE_OUTPUT_STREAM_H__
#define __G_LOCAL_FILE_OUTPUT_STREAM_H__

#include "gfileoutputstream.h"

G_BEGIN_DECLS

#define G_TYPE_LOCAL_FILE_OUTPUT_STREAM         (g_local_file_output_stream_get_type ())
#define G_LOCAL_FILE_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_FILE_OUTPUT_STREAM, GLocalFileOutputStream))
#define G_LOCAL_FILE_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_LOCAL_FILE_OUTPUT_STREAM, GLocalFileOutputStreamClass))
#define G_IS_LOCAL_FILE_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_FILE_OUTPUT_STREAM))
#define G_IS_LOCAL_FILE_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_FILE_OUTPUT_STREAM))
#define G_LOCAL_FILE_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_LOCAL_FILE_OUTPUT_STREAM, GLocalFileOutputStreamClass))

typedef struct _GLocalFileOutputStream         GLocalFileOutputStream;
typedef struct _GLocalFileOutputStreamClass    GLocalFileOutputStreamClass;
typedef struct _GLocalFileOutputStreamPrivate  GLocalFileOutputStreamPrivate;

struct _GLocalFileOutputStream
{
  GFileOutputStream parent;

  /*< private >*/
  GLocalFileOutputStreamPrivate *priv;
};

struct _GLocalFileOutputStreamClass
{
  GFileOutputStreamClass parent_class;
};

typedef enum {
  G_OUTPUT_STREAM_OPEN_MODE_CREATE,
  G_OUTPUT_STREAM_OPEN_MODE_APPEND,
  G_OUTPUT_STREAM_OPEN_MODE_REPLACE
} GOutputStreamOpenMode;

GType g_local_file_output_stream_get_type (void) G_GNUC_CONST;

GFileOutputStream *g_local_file_output_stream_new                (const char             *filename,
								  GOutputStreamOpenMode   open_mode);
void               g_local_file_output_stream_set_original_mtime (GLocalFileOutputStream *stream,
								  time_t                  original_mtime);
void               g_local_file_output_stream_set_create_backup  (GLocalFileOutputStream *stream,
								  gboolean                create_backup);

G_END_DECLS

#endif /* __G_LOCAL_FILE_OUTPUT_STREAM_H__ */
