/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>
#include "gvfsuriutils.h"
#include "gvfsutils.h"
#include <string.h>
#include <stdlib.h>

void
g_vfs_decoded_uri_free (GDecodedUri *decoded)
{
  if (decoded == NULL)
    return;

  g_free (decoded->scheme);
  g_free (decoded->query);
  g_free (decoded->fragment);
  g_free (decoded->userinfo);
  g_free (decoded->host);
  g_free (decoded->path);
  g_free (decoded);
}

GDecodedUri *
g_vfs_decoded_uri_new (void)
{
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);
  uri->port = -1;

  return uri;
}

GDecodedUri *
g_vfs_decode_uri (const char *uri)
{
  GDecodedUri *decoded;
  const char *p, *in, *hier_part_start, *hier_part_end, *query_start, *fragment_start;
  char *out;
  char c;

  /* From RFC 3986 Decodes:
   * URI         = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
   */ 

  p = uri;
  
  /* Decode scheme:
     scheme      = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
  */

  if (!g_ascii_isalpha (*p))
    return NULL;

  while (1)
    {
      c = *p++;

      if (c == ':')
	break;
      
      if (!(g_ascii_isalnum(c) ||
	    c == '+' ||
	    c == '-' ||
	    c == '.'))
	return NULL;
    }

  decoded = g_vfs_decoded_uri_new ();
  
  decoded->scheme = g_malloc (p - uri);
  out = decoded->scheme;
  for (in = uri; in < p - 1; in++)
    *out++ = g_ascii_tolower (*in);
  *out = 0;

  hier_part_start = p;

  query_start = strchr (p, '?');
  if (query_start)
    {
      hier_part_end = query_start++;
      fragment_start = strchr (query_start, '#');
      if (fragment_start)
	{
	  decoded->query = g_strndup (query_start, fragment_start - query_start);
	  decoded->fragment = g_strdup (fragment_start+1);
	}
      else
	{
	  decoded->query = g_strdup (query_start);
	  decoded->fragment = NULL;
	}
    }
  else
    {
      /* No query */
      decoded->query = NULL;
      fragment_start = strchr (p, '#');
      if (fragment_start)
	{
	  hier_part_end = fragment_start++;
	  decoded->fragment = g_strdup (fragment_start);
	}
      else
	{
	  hier_part_end = p + strlen (p);
	  decoded->fragment = NULL;
	}
    }

  /*  3:
      hier-part   = "//" authority path-abempty
                  / path-absolute
                  / path-rootless
                  / path-empty

  */

  if (hier_part_start[0] == '/' &&
      hier_part_start[1] == '/')
    {
      const char *authority_start, *authority_end;
      const char *userinfo_start, *userinfo_end;
      const char *host_start, *host_end;
      const char *port_start;
      
      authority_start = hier_part_start + 2;
      /* authority is always followed by / or nothing */
      authority_end = memchr (authority_start, '/', hier_part_end - authority_start);
      if (authority_end == NULL)
	authority_end = hier_part_end;

      /* 3.2:
	      authority   = [ userinfo "@" ] host [ ":" port ]
      */

      /* Look for the last so that any multiple @ signs are put in the username part.
       * This is not quite correct, as @ should be escaped here, but this happens
       * in practice, so lets handle it the "nicer" way at least. */
      userinfo_end = g_strrstr_len (authority_start,
				    authority_end - authority_start, "@");
      if (userinfo_end)
	{
          char *p;

          host_start = userinfo_end + 1;
          userinfo_start = authority_start;

          /* Applications should not render as clear text any data
           * after the first colon (":") character found within a userinfo
           * subcomponent unless the data after the colon is the empty string
           * (indicating no password). Applications may choose to ignore or
           * reject such data when it is received as part of a reference and
           * should reject the storage of such data in unencrypted form.
           * See https://tools.ietf.org/html/rfc3986
           */
          p = memchr (userinfo_start, ':', userinfo_end - userinfo_start);
          if (p != NULL)
            userinfo_end = p;

	  decoded->userinfo = g_uri_unescape_segment (userinfo_start, userinfo_end, NULL);
	  if (decoded->userinfo == NULL)
	    {
	      g_vfs_decoded_uri_free (decoded);
	      return NULL;
	    }
	}
      else
	host_start = authority_start;

      /* We should handle hostnames in brackets, as those are used by IPv6 URIs
       * See http://tools.ietf.org/html/rfc2732 */
      if (*host_start == '[')
        {
          char *s;

          port_start = NULL;
	  host_end = memchr (host_start, ']', authority_end - host_start);
	  if (host_end == NULL)
	    {
	      g_vfs_decoded_uri_free (decoded);
	      return NULL;
	    }

	  /* Look for the start of the port,
	   * And we sure we don't have it start somewhere
	   * in the path section */
	  s = (char *) host_end;
	  while (1)
	    {
	      if (*s == '/')
	        {
	          port_start = NULL;
	          break;
		}
	      else if (*s == ':')
	        {
	          port_start = s;
	          break;
		}
	      else if (*s == '\0')
	        {
	          break;
		}

	      s++;
	    }
        }
      else
        {
	  port_start = memchr (host_start, ':', authority_end - host_start);
	}

      if (port_start)
	{
	  host_end = port_start++;

	  decoded->port = atoi(port_start);
	}
      else
	{
	  host_end = authority_end;
	  decoded->port = -1;
	}

      /* Let's use the IPv6 address without unescaping. This is needed in order
       * to prevent g_uri_unescape_segment failures when zone identifier
       * separated by the bare % as it is defined by RFC 4007 is used here. The
       * zone identifier should contain just ASCII characters as per RFC 4007,
       * so it doesn't need to be unescaped. I intentionally don't support here
       * what is suggested by RFC 6874, which changes the separator to %25 and
       * at the same time, it suggests that the bare % sign should still be
       * accepted in user interfaces. Such a thing would make this too complex
       * and lead to various problems (e.g. it would not be clear what separator
       * should be used for g_file_get_uri function)...
       */
      if (*host_start == '[')
        decoded->host = g_strndup (host_start, host_end - host_start);
      else
        decoded->host = g_uri_unescape_segment (host_start, host_end, NULL);

      hier_part_start = authority_end;
    }

  decoded->path = g_uri_unescape_segment (hier_part_start, hier_part_end, "/");

  if (decoded->path == NULL)
    {
      g_vfs_decoded_uri_free (decoded);
      return NULL;
    }
  
  return decoded;
}

char *
g_vfs_encode_uri (GDecodedUri *decoded, gboolean allow_utf8)
{
  GString *uri;

  uri = g_string_new (NULL);

  g_string_append (uri, decoded->scheme);
  g_string_append (uri, "://");

  if (decoded->host != NULL)
    {
      if (decoded->userinfo)
	{
	  /* userinfo    = *( unreserved / pct-encoded / sub-delims / ":" ) */
	  g_string_append_uri_escaped (uri, decoded->userinfo,
				       G_URI_RESERVED_CHARS_ALLOWED_IN_USERINFO, allow_utf8);
	  g_string_append_c (uri, '@');
	}

      if (gvfs_is_ipv6 (decoded->host))
        {
          g_string_append (uri, decoded->host);
        }
      else
        {
          g_string_append_uri_escaped (uri, decoded->host,
                                       /* Allowed unescaped in hostname / ip address */
                                       G_URI_RESERVED_CHARS_SUBCOMPONENT_DELIMITERS,
                                       allow_utf8);
        }

      if (decoded->port != -1)
	{
	  g_string_append_c (uri, ':');
	  g_string_append_printf (uri, "%d", decoded->port);
	}
    }

  g_string_append_uri_escaped (uri, decoded->path, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, allow_utf8);
  
  if (decoded->query)
    {
      g_string_append_c (uri, '?');
      g_string_append (uri, decoded->query);
    }
    
  if (decoded->fragment)
    {
      g_string_append_c (uri, '#');
      g_string_append (uri, decoded->fragment);
    }

  return g_string_free (uri, FALSE);
}
