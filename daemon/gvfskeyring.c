/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Carlos Garcia Campos
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Carlos Garcia Campos <carlosgc@gnome.org>
 */

#include <config.h>

#define SECRET_API_SUBJECT_TO_CHANGE 1

#ifdef HAVE_KEYRING
#include <libsecret/secret.h>
#endif

#include "gvfskeyring.h"

gboolean
g_vfs_keyring_is_available (void)
{
#ifdef HAVE_KEYRING
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef HAVE_KEYRING

static void
insert_string (const gchar *key,
	       const gchar *value,
	       GHashTable **attributes)
{
  if (*attributes == NULL)
    return;

  if (!g_utf8_validate (value, -1, NULL))
    {
      g_warning ("Non-utf8 value for key %s\n", key);
      g_hash_table_unref (*attributes);
      *attributes = NULL;
    }

  g_hash_table_insert (*attributes,
		       g_strdup (key),
		       g_strdup (value));
}

static void
insert_int (const gchar *key,
	    gint value,
	    GHashTable **attributes)
{
  if (*attributes == NULL)
    return;

  g_hash_table_insert (*attributes,
		       g_strdup (key),
		       g_strdup_printf ("%d", value));
}

static GHashTable *
build_network_attributes (const gchar *username,
                          const gchar *host,
                          const gchar *domain,
                          const gchar *protocol,
                          const gchar *object,
                          const gchar *authtype,
                          guint32 port)
{
  GHashTable *attributes;

  attributes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (username)
    insert_string ("user", username, &attributes);
  if (host)
    insert_string ("server", host, &attributes);
  if (domain)
    insert_string ("domain", domain, &attributes);
  if (protocol)
    insert_string ("protocol", protocol, &attributes);
  if (object)
    insert_string ("object", object, &attributes);
  if (authtype)
    insert_string ("authtype", authtype, &attributes);
  if (port != 0)
    insert_int ("port", (gint)port, &attributes);

  return attributes;
}

static gchar *
build_network_label (const gchar *user,
                     const gchar *server,
                     const gchar *object,
                     guint32  port)
{
  GString *s;
  gchar *name;

  if (server != NULL)
    {
      s = g_string_new (NULL);
      if (user != NULL)
        {
          g_string_append_uri_escaped (s, user, G_URI_RESERVED_CHARS_ALLOWED_IN_USERINFO, TRUE);
          g_string_append (s, "@");
        }
      g_string_append (s, server);
      if (port != 0)
        g_string_append_printf (s, ":%d", port);
      if (object != NULL)
        g_string_append_printf (s, "/%s", object);
      name = g_string_free (s, FALSE);
    }
  else
    {
      name = g_strdup ("network password");
    }
  return name;
}

static gint
compare_specificity (gconstpointer  a,
		     gconstpointer  b)
{
  GHashTable  *attributes_a, *attributes_b;
  SecretItem  *item_a, *item_b;
  int res;

  item_a = SECRET_ITEM (a);
  attributes_a = secret_item_get_attributes (item_a);

  item_b = SECRET_ITEM (b);
  attributes_b = secret_item_get_attributes (item_b);

  res = g_hash_table_size (attributes_a) - g_hash_table_size (attributes_b);

  /* Prefer the most recent item if they are equal in specificity. */
  if (res == 0)
    res = secret_item_get_modified (item_b) - secret_item_get_modified (item_a);

  g_hash_table_unref (attributes_a);
  g_hash_table_unref (attributes_b);

  return res;
}

#endif /* HAVE_KEYRING */

gboolean
g_vfs_keyring_lookup_password (const gchar *username,
                               const gchar *host,
                               const gchar *domain,
                               const gchar *protocol,
			       const gchar *object,
			       const gchar *authtype,
			       guint32      port, 
                               gchar      **username_out,
                               gchar      **domain_out,
                               gchar      **password_out)
{
#ifdef HAVE_KEYRING
  GHashTable  *attributes;
  SecretItem  *item;
  SecretValue *secret;
  GList       *plist;
  GError      *error = NULL;


  attributes = build_network_attributes (username, host, domain, protocol, object, authtype, port);
  plist = secret_service_search_sync (NULL, SECRET_SCHEMA_COMPAT_NETWORK, attributes,
                                      SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS |
				      SECRET_SEARCH_ALL,
                                      NULL, &error);
  g_hash_table_unref (attributes);

  if (error != NULL)
    {
       g_error_free (error);
       return FALSE;
    }

  if (plist == NULL)
    return FALSE;

  /* We want the least specific result, so we sort the return values.
     For instance, given both items for ftp://host:port and ftp://host
     in the keyring we always want to use the ftp://host one for
     i.e. ftp://host/some/path. */

  plist = g_list_sort (plist, compare_specificity);
  
  item = SECRET_ITEM (plist->data);
  secret = secret_item_get_secret (item);
  attributes = secret_item_get_attributes (item);
  g_list_free_full (plist, g_object_unref);

  if (secret == NULL)
    {
      if (attributes)
        g_hash_table_unref (attributes);
      return FALSE;
    }

  *password_out = g_strdup (secret_value_get (secret, NULL));
  secret_value_unref (secret);

  if (username_out)
    *username_out = g_strdup (g_hash_table_lookup (attributes, "user"));

  if (domain_out)
    *domain_out = g_strdup (g_hash_table_lookup (attributes, "domain"));

  g_hash_table_unref (attributes);
  return TRUE;
#else
  return FALSE;
#endif /* HAVE_KEYRING */
}

gboolean
g_vfs_keyring_save_password (const gchar  *username,
                             const gchar  *host,
                             const gchar  *domain,
                             const gchar  *protocol,
			     const gchar  *object,
			     const gchar  *authtype,
			     guint32       port,
                             const gchar  *password,
                             GPasswordSave flags)
{
#ifdef HAVE_KEYRING
  const gchar       *keyring;
  GHashTable        *attributes;
  gchar             *label;
  gboolean           ret;

  if (flags == G_PASSWORD_SAVE_NEVER)
    return FALSE;

  keyring = (flags == G_PASSWORD_SAVE_FOR_SESSION) ? SECRET_COLLECTION_SESSION : SECRET_COLLECTION_DEFAULT;

  label = build_network_label (username, host, object, port);
  attributes = build_network_attributes (username, host, domain, protocol, object, authtype, port);

  ret = secret_password_storev_sync (SECRET_SCHEMA_COMPAT_NETWORK, attributes,
                                     keyring, label, password, NULL, NULL);

  g_free (label);
  g_hash_table_unref (attributes);

  return ret;
#else
  return FALSE;
#endif /* HAVE_KEYRING */
}
