#ifndef __G_FILE_DAEMON_H__
#define __G_FILE_DAEMON_H__

#include <gio/gfile.h>
#include "gvfsimpldaemon.h"

G_BEGIN_DECLS

#define G_TYPE_FILE_DAEMON         (g_file_daemon_get_type ())
#define G_FILE_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_DAEMON, GFileDaemon))
#define G_FILE_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_DAEMON, GFileDaemonClass))
#define G_IS_FILE_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_DAEMON))
#define G_IS_FILE_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_DAEMON))
#define G_FILE_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_DAEMON, GFileDaemonClass))

typedef struct _GFileDaemon        GFileDaemon;
typedef struct _GFileDaemonClass   GFileDaemonClass;

struct _GFileDaemonClass
{
  GObjectClass parent_class;
};

GType g_file_daemon_get_type (void) G_GNUC_CONST;
  
GFile * g_file_daemon_new (GQuark match_bus_name,
			   const char *path);

G_END_DECLS

#endif /* __G_FILE_DAEMON_H__ */
