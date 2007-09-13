#ifndef __G_FILE_OUTPUT_STREAM_LOCAL_H__
#define __G_FILE_OUTPUT_STREAM_LOCAL_H__

#include <gvfs/gfileoutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_OUTPUT_STREAM_LOCAL         (g_file_output_stream_local_get_type ())
#define G_FILE_OUTPUT_STREAM_LOCAL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_OUTPUT_STREAM_LOCAL, GFileOutputStreamLocal))
#define G_FILE_OUTPUT_STREAM_LOCAL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_OUTPUT_STREAM_LOCAL, GFileOutputStreamLocalClass))
#define G_IS_FILE_OUTPUT_STREAM_LOCAL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_OUTPUT_STREAM_LOCAL))
#define G_IS_FILE_OUTPUT_STREAM_LOCAL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_OUTPUT_STREAM_LOCAL))
#define G_FILE_OUTPUT_STREAM_LOCAL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_OUTPUT_STREAM_LOCAL, GFileOutputStreamLocalClass))

typedef struct _GFileOutputStreamLocal         GFileOutputStreamLocal;
typedef struct _GFileOutputStreamLocalClass    GFileOutputStreamLocalClass;
typedef struct _GFileOutputStreamLocalPrivate  GFileOutputStreamLocalPrivate;

struct _GFileOutputStreamLocal
{
  GFileOutputStream parent;

  /*< private >*/
  GFileOutputStreamLocalPrivate *priv;
};

struct _GFileOutputStreamLocalClass
{
  GFileOutputStreamClass parent_class;
};

typedef enum {
  G_OUTPUT_STREAM_OPEN_MODE_CREATE,
  G_OUTPUT_STREAM_OPEN_MODE_APPEND,
  G_OUTPUT_STREAM_OPEN_MODE_REPLACE
} GOutputStreamOpenMode;

GType g_file_output_stream_local_get_type (void) G_GNUC_CONST;

GFileOutputStream *g_file_output_stream_local_new                (const char             *filename,
								  GOutputStreamOpenMode   open_mode);
void               g_file_output_stream_local_set_original_mtime (GFileOutputStreamLocal *stream,
								  time_t                  original_mtime);
void               g_file_output_stream_local_set_create_backup  (GFileOutputStreamLocal *stream,
								  gboolean                create_backup);

G_END_DECLS

#endif /* __G_FILE_OUTPUT_STREAM_LOCAL_H__ */
