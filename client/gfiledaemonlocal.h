#ifndef __G_FILE_DAEMON_LOCAL_H__
#define __G_FILE_DAEMON_LOCAL_H__

#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_DAEMON_LOCAL         (g_file_daemon_local_get_type ())
#define G_FILE_DAEMON_LOCAL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_DAEMON_LOCAL, GFileDaemonLocal))
#define G_FILE_DAEMON_LOCAL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_DAEMON_LOCAL, GFileDaemonLocalClass))
#define G_IS_FILE_DAEMON_LOCAL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_DAEMON_LOCAL))
#define G_IS_FILE_DAEMON_LOCAL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_DAEMON_LOCAL))
#define G_FILE_DAEMON_LOCAL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_DAEMON_LOCAL, GFileDaemonLocalClass))

typedef struct _GFileDaemonLocal        GFileDaemonLocal;
typedef struct _GFileDaemonLocalClass   GFileDaemonLocalClass;

struct _GFileDaemonLocalClass
{
  GObjectClass parent_class;
};

GType g_file_daemon_local_get_type (void) G_GNUC_CONST;
  
GFile * g_file_daemon_local_new (GFile *wrapped);

G_END_DECLS


#endif /* __G_FILE_DAEMON_LOCAL_H__ */
