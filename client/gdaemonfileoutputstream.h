#ifndef __G_DAEMON_FILE_OUTPUT_STREAM_H__
#define __G_DAEMON_FILE_OUTPUT_STREAM_H__

#include <gio/gfileoutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_OUTPUT_STREAM         (g_daemon_file_output_stream_get_type ())
#define G_DAEMON_FILE_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStream))
#define G_DAEMON_FILE_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStreamClass))
#define G_IS_DAEMON_FILE_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM))
#define G_IS_DAEMON_FILE_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_OUTPUT_STREAM))
#define G_DAEMON_FILE_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStreamClass))

typedef struct _GDaemonFileOutputStream         GDaemonFileOutputStream;
typedef struct _GDaemonFileOutputStreamClass    GDaemonFileOutputStreamClass;

struct _GDaemonFileOutputStreamClass
{
  GFileOutputStreamClass parent_class;
};

GType g_daemon_file_output_stream_get_type (void) G_GNUC_CONST;

GFileOutputStream *g_daemon_file_output_stream_new (int fd,
						    gboolean can_seek,
						    goffset initial_offset);

G_END_DECLS

#endif /* __G_DAEMON_FILE_OUTPUT_STREAM_H__ */
