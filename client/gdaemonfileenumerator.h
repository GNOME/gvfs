#ifndef __G_DAEMON_FILE_ENUMERATOR_H__
#define __G_DAEMON_FILE_ENUMERATOR_H__

#include <gio/gfileenumerator.h>
#include <gio/gfileinfo.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_ENUMERATOR         (g_daemon_file_enumerator_get_type ())
#define G_DAEMON_FILE_ENUMERATOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumerator))
#define G_DAEMON_FILE_ENUMERATOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumeratorClass))
#define G_IS_DAEMON_FILE_ENUMERATOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_ENUMERATOR))
#define G_IS_DAEMON_FILE_ENUMERATOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_ENUMERATOR))
#define G_DAEMON_FILE_ENUMERATOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumeratorClass))

typedef struct _GDaemonFileEnumerator         GDaemonFileEnumerator;
typedef struct _GDaemonFileEnumeratorClass    GDaemonFileEnumeratorClass;
typedef struct _GDaemonFileEnumeratorPrivate  GDaemonFileEnumeratorPrivate;

struct _GDaemonFileEnumeratorClass
{
  GFileEnumeratorClass parent_class;
};

GType g_daemon_file_enumerator_get_type (void) G_GNUC_CONST;

GDaemonFileEnumerator *g_daemon_file_enumerator_new                 (void);
char  *                g_daemon_file_enumerator_get_object_path     (GDaemonFileEnumerator *enumerator);
void                   g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
								     DBusConnection        *connection);


G_END_DECLS

#endif /* __G_FILE_DAEMON_FILE_ENUMERATOR_H__ */
