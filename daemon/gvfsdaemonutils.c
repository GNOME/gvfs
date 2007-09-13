#include <config.h>

#include "gvfsdaemonutils.h"

DBusMessage *
dbus_message_new_error_from_gerror (DBusMessage *message,
				    GError *error)
{
  char *error_name;
  DBusMessage *reply;
    
  error_name = g_strdup_printf ("org.glib.GError.%s.%d",
				g_quark_to_string (error->domain),
				error->code);
  reply = dbus_message_new_error (message, error_name, error->message);
  g_free (error_name);
  return reply;
}
