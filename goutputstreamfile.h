#ifndef __G_OUTPUT_STREAM_FILE_H__
#define __G_OUTPUT_STREAM_FILE_H__

#include "goutputstream.h"

G_BEGIN_DECLS

#define G_TYPE_OUTPUT_STREAM_FILE         (g_output_stream_file_get_type ())
#define G_OUTPUT_STREAM_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_OUTPUT_STREAM_FILE, GOutputStreamFile))
#define G_OUTPUT_STREAM_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_OUTPUT_STREAM_FILE, GOutputStreamFileClass))
#define G_IS_OUTPUT_STREAM_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_OUTPUT_STREAM_FILE))
#define G_IS_OUTPUT_STREAM_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_OUTPUT_STREAM_FILE))
#define G_OUTPUT_STREAM_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_OUTPUT_STREAM_FILE, GOutputStreamFileClass))

typedef struct _GOutputStreamFile         GOutputStreamFile;
typedef struct _GOutputStreamFileClass    GOutputStreamFileClass;
typedef struct _GOutputStreamFilePrivate  GOutputStreamFilePrivate;

struct _GOutputStreamFile
{
  GOutputStream parent;

  /*< private >*/
  GOutputStreamFilePrivate *priv;
};

struct _GOutputStreamFileClass
{
  GOutputStreamClass parent_class;
};

typedef enum {
  G_OUTPUT_STREAM_FILE_OPEN_CREATE,
  G_OUTPUT_STREAM_FILE_OPEN_APPEND,
  G_OUTPUT_STREAM_FILE_OPEN_REPLACE
} GOutputStreamFileOpenMode;

GType g_output_stream_file_get_type (void) G_GNUC_CONST;

GOutputStream *g_output_stream_file_new                (const char                *filename,
							GOutputStreamFileOpenMode  open_mode);
void           g_output_stream_file_set_original_mtime (GOutputStream             *stream,
							time_t                     original_mtime);
void           g_output_stream_file_set_create_backup  (GOutputStream             *stream,
							gboolean                   create_backup);

G_END_DECLS

#endif /* __G_OUTPUT_STREAM_FILE_H__ */
