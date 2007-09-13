#ifndef __G_FILE_OUTPUT_STREAM_DAEMON_H__
#define __G_FILE_OUTPUT_STREAM_DAEMON_H__

#include <gio/gfileoutputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_OUTPUT_STREAM_DAEMON         (g_file_output_stream_daemon_get_type ())
#define G_FILE_OUTPUT_STREAM_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_OUTPUT_STREAM_DAEMON, GFileOutputStreamDaemon))
#define G_FILE_OUTPUT_STREAM_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_OUTPUT_STREAM_DAEMON, GFileOutputStreamDaemonClass))
#define G_IS_FILE_OUTPUT_STREAM_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_OUTPUT_STREAM_DAEMON))
#define G_IS_FILE_OUTPUT_STREAM_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_OUTPUT_STREAM_DAEMON))
#define G_FILE_OUTPUT_STREAM_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_OUTPUT_STREAM_DAEMON, GFileOutputStreamDaemonClass))

typedef struct _GFileOutputStreamDaemon         GFileOutputStreamDaemon;
typedef struct _GFileOutputStreamDaemonClass    GFileOutputStreamDaemonClass;

struct _GFileOutputStreamDaemonClass
{
  GFileOutputStreamClass parent_class;
};

GType g_file_output_stream_daemon_get_type (void) G_GNUC_CONST;

GFileOutputStream *g_file_output_stream_daemon_new (int fd,
						    gboolean can_seek,
						    goffset initial_offset);

G_END_DECLS

#endif /* __G_FILE_OUTPUT_STREAM_DAEMON_H__ */
