#ifndef __G_FILE_ENUMERATOR_DAEMON_H__
#define __G_FILE_ENUMERATOR_DAEMON_H__

#include <gio/gfileenumerator.h>
#include <gio/gfileinfo.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_ENUMERATOR_DAEMON         (g_file_enumerator_daemon_get_type ())
#define G_FILE_ENUMERATOR_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_ENUMERATOR_DAEMON, GFileEnumeratorDaemon))
#define G_FILE_ENUMERATOR_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_ENUMERATOR_DAEMON, GFileEnumeratorDaemonClass))
#define G_IS_FILE_ENUMERATOR_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_ENUMERATOR_DAEMON))
#define G_IS_FILE_ENUMERATOR_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_ENUMERATOR_DAEMON))
#define G_FILE_ENUMERATOR_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_ENUMERATOR_DAEMON, GFileEnumeratorDaemonClass))

typedef struct _GFileEnumeratorDaemon         GFileEnumeratorDaemon;
typedef struct _GFileEnumeratorDaemonClass    GFileEnumeratorDaemonClass;
typedef struct _GFileEnumeratorDaemonPrivate  GFileEnumeratorDaemonPrivate;

struct _GFileEnumeratorDaemonClass
{
  GFileEnumeratorClass parent_class;
};

GType g_file_enumerator_daemon_get_type (void) G_GNUC_CONST;

GFileEnumeratorDaemon *g_file_enumerator_daemon_new                 (void);
char  *                g_file_enumerator_daemon_get_object_path     (GFileEnumeratorDaemon *enumerator);
void                   g_file_enumerator_daemon_set_sync_connection (GFileEnumeratorDaemon *enumerator,
								     DBusConnection        *connection);
void                   g_file_enumerator_daemon_set_request_flags   (GFileEnumeratorDaemon *enumerator,
								     GFileInfoRequestFlags  flags);


G_END_DECLS

#endif /* __G_FILE_FILE_ENUMERATOR_DAEMON_H__ */
