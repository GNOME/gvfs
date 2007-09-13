#ifndef __G_DAEMON_FILE_H__
#define __G_DAEMON_FILE_H__

#include <gio/gfile.h>
#include "gdaemonvfs.h"
#include "gmountspec.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE         (g_daemon_file_get_type ())
#define G_DAEMON_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE, GDaemonFile))
#define G_DAEMON_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE, GDaemonFileClass))
#define G_IS_DAEMON_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE))
#define G_IS_DAEMON_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE))
#define G_DAEMON_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE, GDaemonFileClass))

typedef struct _GDaemonFile        GDaemonFile;
typedef struct _GDaemonFileClass   GDaemonFileClass;

struct _GDaemonFileClass
{
  GObjectClass parent_class;
};

GType g_daemon_file_get_type (void) G_GNUC_CONST;
  
GFile * g_daemon_file_new (GMountSpec *mount_spec,
			   const char *path);

G_END_DECLS

#endif /* __G_DAEMON_FILE_H__ */
