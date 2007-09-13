#ifndef __G_FILE_INPUT_STREAM_DAEMON_H__
#define __G_FILE_INPUT_STREAM_DAEMON_H__

#include <gvfs/gfileinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_INPUT_STREAM_DAEMON         (g_file_input_stream_daemon_get_type ())
#define G_FILE_INPUT_STREAM_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_INPUT_STREAM_DAEMON, GFileInputStreamDaemon))
#define G_FILE_INPUT_STREAM_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_INPUT_STREAM_DAEMON, GFileInputStreamDaemonClass))
#define G_IS_FILE_INPUT_STREAM_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_INPUT_STREAM_DAEMON))
#define G_IS_FILE_INPUT_STREAM_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_INPUT_STREAM_DAEMON))
#define G_FILE_INPUT_STREAM_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_INPUT_STREAM_DAEMON, GFileInputStreamDaemonClass))

typedef struct _GFileInputStreamDaemon         GFileInputStreamDaemon;
typedef struct _GFileInputStreamDaemonClass    GFileInputStreamDaemonClass;
typedef struct _GFileInputStreamDaemonPrivate  GFileInputStreamDaemonPrivate;

struct _GFileInputStreamDaemon
{
  GFileInputStream parent;

  /*< private >*/
  GFileInputStreamDaemonPrivate *priv;
};

struct _GFileInputStreamDaemonClass
{
  GFileInputStreamClass parent_class;
};

GType g_file_input_stream_daemon_get_type (void) G_GNUC_CONST;

GFileInputStream *g_file_input_stream_daemon_new (const char *filename,
						const char *mountpoint);

G_END_DECLS

#endif /* __G_FILE_INPUT_STREAM_DAEMON_H__ */
