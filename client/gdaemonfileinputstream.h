#ifndef __G_DAEMON_FILE_INPUT_STREAM_H__
#define __G_DAEMON_FILE_INPUT_STREAM_H__

#include <gio/gfileinputstream.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_INPUT_STREAM         (g_daemon_file_input_stream_get_type ())
#define G_DAEMON_FILE_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_INPUT_STREAM, GDaemonFileInputStream))
#define G_DAEMON_FILE_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE_INPUT_STREAM, GDaemonFileInputStreamClass))
#define G_IS_DAEMON_FILE_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_INPUT_STREAM))
#define G_IS_DAEMON_FILE_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_INPUT_STREAM))
#define G_DAEMON_FILE_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE_INPUT_STREAM, GDaemonFileInputStreamClass))

typedef struct _GDaemonFileInputStream         GDaemonFileInputStream;
typedef struct _GDaemonFileInputStreamClass    GDaemonFileInputStreamClass;

struct _GDaemonFileInputStreamClass
{
  GFileInputStreamClass parent_class;
};

GType g_daemon_file_input_stream_get_type (void) G_GNUC_CONST;

GFileInputStream *g_daemon_file_input_stream_new (int fd,
						  gboolean can_seek);

G_END_DECLS

#endif /* __G_DAEMON_FILE_INPUT_STREAM_H__ */
