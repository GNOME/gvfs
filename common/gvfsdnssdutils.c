/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-glib/glib-watch.h>
#include <avahi-glib/glib-malloc.h>

#include "gvfsdnssdutils.h"

static gchar *
escape_service_name (const gchar *service_name)
{
  GString *s;
  char *res;
  const gchar *p;

  g_return_val_if_fail (service_name != NULL, NULL);

  s = g_string_new (NULL);

  p = service_name;
  while (*p != '\0')
    {
      if (*p == '\\')
        g_string_append (s, "\\\\");
      else if (*p == '.')
        g_string_append (s, "\\.");
      else if (*p == '/')
        g_string_append (s, "\\s");
      else
        g_string_append_c (s, *p);
      p++;
    }

  res = g_uri_escape_string (s->str, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
  g_string_free (s, TRUE);
  return res;
}

static gchar *
escape_service_name2 (const gchar *service_name)
{
  GString *s;
  const gchar *p;

  g_return_val_if_fail (service_name != NULL, NULL);

  s = g_string_new (NULL);

  p = service_name;
  while (*p != '\0')
    {
      if (*p == '.')
        g_string_append (s, "%2e");
      else
        g_string_append_c (s, *p);
      p++;
    }

  return g_string_free (s, FALSE);
}

/**
 * g_vfs_get_dns_sd_uri_for_triple:
 * @service_name: DNS-SD service name.
 * @service_type: DNS-SD service type.
 * @domain: DNS-SD domain.
 *
 * Creates an URI for a file on the GVfs <literal>dns-sd</literal>
 * virtual file system that provides live data for resolving the given
 * DNS-SD service.
 *
 * The URI is of the form
 * <literal>dns-sd://domain/service_name.service_type<literal> with
 * suitable encoding added.
 *
 * Note that there may not exist a file at the returned URI, the
 * resource providing the DNS-SD service will have to be available for
 * the file to exist.
 *
 * Returns: An URI. Free with g_free().
 **/
gchar *
g_vfs_get_dns_sd_uri_for_triple (const gchar *service_name,
                                 const gchar *service_type,
                                 const gchar *domain)
{
  gchar *escaped_service_name;
  gchar *ret;

  g_return_val_if_fail (service_name != NULL, NULL);
  g_return_val_if_fail (service_type != NULL, NULL);
  g_return_val_if_fail (domain != NULL, NULL);

  escaped_service_name = escape_service_name (service_name);
  
  ret = g_strdup_printf ("dns-sd://%s/%s.%s",
                         domain,
                         escaped_service_name,
                         service_type);
  g_free (escaped_service_name);

  return ret;
}

/**
 * g_vfs_encode_dns_sd_triple:
 * @service_name: DNS-SD service name.
 * @service_type: DNS-SD service type.
 * @domain: DNS-SD domain.
 *
 * Creates an encoded triple representing a DNS-SD service. The triple
 * will be of the form
 * <literal>service_name.service_type.domain</literal> with suitable
 * encoding.
 *
 * Use g_vfs_decode_dns_sd_triple() to decode the returned string.
 *
 * Returns: A string representing the triple, free with g_free().
 **/
gchar *
g_vfs_encode_dns_sd_triple (const gchar *service_name,
                            const gchar *service_type,
                            const gchar *domain)
{
  char *dot_escaped_service_name;
  char *escaped_service_name;
  char *escaped_service_type;
  char *escaped_domain;
  char *s;

  escaped_service_name = g_uri_escape_string (service_name, NULL, FALSE);
  dot_escaped_service_name = escape_service_name2 (escaped_service_name);
  escaped_service_type = g_uri_escape_string (service_type, NULL, FALSE);
  escaped_domain = g_uri_escape_string (domain, NULL, FALSE);
  s = g_strdup_printf ("%s.%s.%s",
                       dot_escaped_service_name,
                       escaped_service_type,
                       escaped_domain);
  g_free (dot_escaped_service_name);
  g_free (escaped_service_name);
  g_free (escaped_service_type);
  g_free (escaped_domain);
  return s;
}

/**
 * g_vfs_decode_dns_sd_triple:
 * @encoded_triple: A string obtained from g_vfs_encode_dns_sd_triple().
 * @out_service_name: %NULL or return location for the service name.
 * @out_service_type: %NULL or return location for the service type.
 * @out_domain: %NULL or return location for the domain.
 * @error: Return location for error or %NULL.
 *
 * Constructs a DNS-SD triple by decoding a string generated from
 * g_vfs_encode_dns_sd_triple(). This can fail if @encoded_triple is
 * malformed.
 *
 * Returns: %TRUE unless @error is set.
 **/
gboolean
g_vfs_decode_dns_sd_triple (const gchar *encoded_triple,
                            gchar      **out_service_name,
                            gchar      **out_service_type,
                            gchar      **out_domain,
                            GError     **error)
{
  gboolean ret;
  int n;
  int m;
  int service_type_pos;
  char *escaped_service_name;
  char *escaped_service_type;
  char *escaped_domain;

  g_return_val_if_fail (encoded_triple != NULL, FALSE);


  escaped_service_name = NULL;
  escaped_service_type = NULL;
  escaped_domain = NULL;
  ret = FALSE;

  if (out_service_name != NULL)
    *out_service_name = NULL;

  if (out_service_type != NULL)
    *out_service_type = NULL;

  if (out_domain != NULL)
    *out_domain = NULL;

  /* Find first '.' followed by an underscore. */
  for (n = 0; encoded_triple[n] != '\0'; n++)
    {
      if (encoded_triple[n] == '.')
        {
          if (encoded_triple[n + 1] == '_')
            break;
        }
    }
  if (encoded_triple[n] == '\0')
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   _("Malformed DNS-SD encoded_triple “%s”"),
                   encoded_triple);
      goto out;
    }

  escaped_service_name = g_strndup (encoded_triple, n);
  if (escaped_service_name == NULL)
    goto out;

  if (out_service_name != NULL)
    *out_service_name = g_uri_unescape_string (escaped_service_name, NULL);

  /* skip dot between service name and service type */
  n += 1;

  service_type_pos = n;

  /* skip next two dots */
  for (m = 0; m < 2; m++)
    {
      for (; encoded_triple[n] != '\0'; n++)
        {
          if (encoded_triple[n] == '.')
            break;
        }
      if (encoded_triple[n] == '\0')
        {
          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_INVALID_ARGUMENT,
                       _("Malformed DNS-SD encoded_triple “%s”"),
                       encoded_triple);
          goto out;
        }
      n++;
    }

  escaped_service_type = g_strndup (encoded_triple + service_type_pos, n - service_type_pos - 1);
  if (out_service_type != NULL)
    *out_service_type = g_uri_unescape_string (escaped_service_type, NULL);

  /* the domain is the rest */
  if (encoded_triple[n] == '\0')
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   _("Malformed DNS-SD encoded_triple “%s”"),
                   encoded_triple);
      goto out;
    }

  escaped_domain = g_strdup (encoded_triple + n);
  if (out_domain != NULL)
    *out_domain = g_uri_unescape_string (escaped_domain, NULL);

  ret = TRUE;

 out:
  g_free (escaped_service_name);
  g_free (escaped_service_type);
  g_free (escaped_domain);
  return ret;
}

gchar *
g_vfs_normalize_encoded_dns_sd_triple (const gchar *encoded_triple)
{
  char *service_name;
  char *service_type;
  char *domain;
  char *ret;

  ret = NULL;

  if (!g_vfs_decode_dns_sd_triple (encoded_triple,
                                   &service_name,
                                   &service_type,
                                   &domain,
                                   NULL))
    goto out;

  ret = g_vfs_encode_dns_sd_triple (service_name, service_type, domain);
  g_free (service_name);
  g_free (service_type);
  g_free (domain);

 out:
  return ret;
}

