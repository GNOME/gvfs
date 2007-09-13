#include <config.h>
#include <gvfsmountpoint.h>
#include <gvfsdaemonutils.h>

GVfsMountpoint *
g_vfs_mountpoint_copy (GVfsMountpoint  *mountpoint)
{
  GVfsMountpoint *copy;

  copy = g_new (GVfsMountpoint, 1);
  copy->method = g_strdup (mountpoint->method);
  copy->user = g_strdup (mountpoint->user);
  copy->host = g_strdup (mountpoint->host);
  copy->port = mountpoint->port;
  copy->path = g_strdup (mountpoint->path);

  return copy;
}

void
g_vfs_mountpoint_free (GVfsMountpoint  *mountpoint)
{
  g_free (mountpoint->method);
  g_free (mountpoint->user);
  g_free (mountpoint->host);
  g_free (mountpoint->path);
  g_free (mountpoint);
}

GVfsMountpoint *
g_vfs_mountpoint_from_dbus (DBusMessageIter *iter)
{
  return NULL;
}
  
void
g_vfs_mountpoint_to_dbus (GVfsMountpoint  *mountpoint,
			  DBusMessageIter *iter)
{
  dbus_bool_t res;
  dbus_int32_t port;

  res = TRUE;
  res &= dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING,
					 &mountpoint->method);
  
  /* TODO: Is this always utf8? */
  res &= dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING,
					 &mountpoint->user);
  
  res &= dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING,
					 &mountpoint->host);
  
  port = mountpoint->port;
  res &= dbus_message_iter_append_basic (iter, DBUS_TYPE_INT32, &port);
  
  res &= _g_dbus_message_iter_append_filename (iter, mountpoint->path);
  
  if (!res)
    g_error ("out of memory");
}
