#include <config.h>

#include <string.h>

#include "gfileref.h"
#include "gdbusutils.h"

static int
safe_strcmp (const char *a, const char *b)
{
  if (a == b)
    return 0;
  if (a == NULL)
    return -1;
  if (b == NULL)
    return 1;
  return strcmp (a, b);
}

void
g_file_ref_free (GFileRef *ref)
{
  g_free (ref->protocol);
  g_free (ref->username);
  g_free (ref->host);
  g_free (ref->path);
  g_free (ref);
}

void
g_file_ref_template_free (GFileRefTemplate *template)
{
  g_free (template->protocol);
  g_free (template->username);
  g_free (template->host);
  g_free (template->path_prefix);
  g_free (template);
}

gboolean
g_file_ref_template_matches (GFileRefTemplate *template,
			     GFileRef         *ref)
{
  const char *s;
  int depth;
  
  if (template->protocol != NULL &&
      safe_strcmp (template->protocol, ref->protocol) != 0)
    return FALSE;

  if (template->username != NULL &&
      safe_strcmp (template->username, ref->username) != 0)
    return FALSE;

  if (template->host != NULL &&
      safe_strcmp (template->host, ref->host) != 0)
    return FALSE;
  
  if (template->port != G_FILE_REF_PORT_ANY &&
      template->port != ref->port)
    return FALSE;

  if (template->path_prefix != NULL)
    {
      int prefix_len = strlen (template->path_prefix);

      if (strlen (ref->path) < prefix_len ||
	  strncmp (template->path_prefix, ref->path, prefix_len) != 0)
	return FALSE;
      /* Must template whole dirname */
      if (ref->path[prefix_len] != 0 &&
	  ref->path[prefix_len] != '/')
	return FALSE;
    }

  if (template->max_path_depth > 0 ||
      template->min_path_depth > 0 )
    {
      s = ref->path;
      depth = 0;
      while ((s = strchr (s, '/')) != NULL)
	{
	  /* Don't count final slash or consecutive slashes */
	  if (s[1] != 0 &&
	      s[1] != '/')
	    depth++;
	}
      if (template->max_path_depth > 0 &&
	  depth > template->max_path_depth)
	return FALSE;
      
      if (depth < template->min_path_depth)
	return FALSE;
    }
  
  return TRUE;
}

gboolean
g_file_ref_template_equal (GFileRefTemplate *a,
			   GFileRefTemplate *b)
{
  if (safe_strcmp (a->protocol, b->protocol) != 0)
    return FALSE;

  if (safe_strcmp (a->username, b->username) != 0)
    return FALSE;

  if (safe_strcmp (a->host, b->host) != 0)
    return FALSE;
  
  if (a->port != b->port)
    return FALSE;

  if (safe_strcmp (a->path_prefix, b->path_prefix) != 0)
    return FALSE;
  
  if (a->max_path_depth != b->max_path_depth)
    return FALSE;

  if (a->min_path_depth != b->min_path_depth)
    return FALSE;

  return TRUE;
}

GFileRefTemplate *
g_file_ref_template_from_dbus (DBusMessageIter  *iter)
{
  DBusMessageIter struct_iter;
  const char *protocol, *username, *host, *path_prefix;
  int protocol_len, username_len, host_len, path_prefix_len;
  GFileRefTemplate *template;
  gint32 port, max_path_depth, min_path_depth;
  dbus_bool_t has_protocol, has_username, has_host, has_path_prefix;
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    return NULL;

  dbus_message_iter_recurse (iter, &struct_iter);

  template = NULL;

  if (_g_dbus_message_iter_get_args (iter,
				     NULL,
				     DBUS_TYPE_BOOLEAN, &has_protocol,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &protocol, &protocol_len,
				     DBUS_TYPE_BOOLEAN, &has_username,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &username, &username_len,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     DBUS_TYPE_BOOLEAN, &has_host,
				     &host, &host_len,
				     DBUS_TYPE_INT32, &port,
				     DBUS_TYPE_BOOLEAN, &has_path_prefix,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &path_prefix, &path_prefix_len,
				     DBUS_TYPE_INT32, &max_path_depth,
				     DBUS_TYPE_INT32, &min_path_depth,
				     0))
    {
      template = g_new0 (GFileRefTemplate, 1);
      if (has_protocol)
	template->protocol = g_strndup (protocol, protocol_len);
      if (has_username)
	template->username = g_strndup (username, username_len);
      if (has_host)
	template->host = g_strndup (host, host_len);
      template->port = port;
      if (has_path_prefix)
	template->path_prefix = g_strndup (path_prefix, path_prefix_len);
      template->max_path_depth = max_path_depth;
      template->min_path_depth = min_path_depth;
    }
  return template;
}
