#ifndef __G_LOCAL_DAEMON_FILE_H__
#define __G_LOCAL_DAEMON_FILE_H__

#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_DAEMON_FILE         (g_local_daemon_file_get_type ())
#define G_LOCAL_DAEMON_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_DAEMON_FILE, GLocalDaemonFile))
#define G_LOCAL_DAEMON_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_LOCAL_DAEMON_FILE, GLocalDaemonFileClass))
#define G_IS_LOCAL_DAEMON_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_DAEMON_FILE))
#define G_IS_LOCAL_DAEMON_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_DAEMON_FILE))
#define G_LOCAL_DAEMON_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_LOCAL_DAEMON_FILE, GLocalDaemonFileClass))

typedef struct _GLocalDaemonFile        GLocalDaemonFile;
typedef struct _GLocalDaemonFileClass   GLocalDaemonFileClass;

struct _GLocalDaemonFileClass
{
  GObjectClass parent_class;
};

GType g_local_daemon_file_get_type (void) G_GNUC_CONST;
  
GFile * g_local_daemon_file_new (GFile *wrapped);

G_END_DECLS


#endif /* __G_LOCAL_DAEMON_FILE_H__ */
