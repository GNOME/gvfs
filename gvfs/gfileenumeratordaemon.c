#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileenumeratordaemon.h>
#include <gfileinfo.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
volatile gint path_counter = 1;

struct _GFileEnumeratorDaemon
{
  GFileEnumerator parent;

  gint id;
};

G_DEFINE_TYPE (GFileEnumeratorDaemon, g_file_enumerator_daemon, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *g_file_enumerator_daemon_next_file (GFileEnumerator  *enumerator,
						      GCancellable     *cancellable,
						      GError          **error);
static gboolean   g_file_enumerator_daemon_stop      (GFileEnumerator  *enumerator,
						      GCancellable     *cancellable,
						      GError          **error);

static void
g_file_enumerator_daemon_finalize (GObject *object)
{
  GFileEnumeratorDaemon *daemon;

  daemon = G_FILE_ENUMERATOR_DAEMON (object);

  if (G_OBJECT_CLASS (g_file_enumerator_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_daemon_parent_class)->finalize) (object);
}


static void
g_file_enumerator_daemon_class_init (GFileEnumeratorDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_file_enumerator_daemon_finalize;

  enumerator_class->next_file = g_file_enumerator_daemon_next_file;
  enumerator_class->stop = g_file_enumerator_daemon_stop;
}

static void
g_file_enumerator_daemon_init (GFileEnumeratorDaemon *daemon)
{
  daemon->id = g_atomic_int_exchange_and_add (&path_counter, 1);
}

GFileEnumeratorDaemon *
g_file_enumerator_daemon_new (void)
{
  GFileEnumeratorDaemon *daemon;

  daemon = g_object_new (G_TYPE_FILE_ENUMERATOR_DAEMON, NULL);
  
  return daemon;
}

char  *
g_file_enumerator_daemon_get_object_path (GFileEnumeratorDaemon *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_file_enumerator_daemon_set_sync_connection (GFileEnumeratorDaemon *enumerator,
					      DBusConnection        *connection)
{
  /* TODO */
}

GFileEnumeratorDaemon *
g_get_file_enumerator_daemon_from_path (const char *path)
{
  int id;
  
  if (!g_str_has_prefix (path, OBJ_PATH_PREFIX))
      return NULL;

  id = atoi (path + strlen ("/org/gtk/vfs/client/enumerator/"));

  return NULL;
}

void
g_file_enumerator_daemon_dispatch_message (GFileEnumeratorDaemon *enumerator,
					   DBusMessage           *message)
{
  
}

static GFileInfo *
g_file_enumerator_daemon_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  /* GFileEnumeratorDaemon *daemon = G_FILE_ENUMERATOR_DAEMON (enumerator); */
  return NULL;
}

static gboolean
g_file_enumerator_daemon_stop (GFileEnumerator *enumerator,
			      GCancellable     *cancellable,
			      GError          **error)
{
  /*GFileEnumeratorDaemon *daemon = G_FILE_ENUMERATOR_DAEMON (enumerator); */

  return TRUE;
}


