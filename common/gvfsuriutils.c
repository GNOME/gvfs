#include <config.h>
#include "gvfsuriutils.h"
#include <string.h>
#include <stdlib.h>

void
g_decoded_uri_free (GDecodedUri *decoded)
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
g_decoded_uri_new (void)
{
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);
  uri->port = -1;

  return uri;
}

GDecodedUri *
g_decode_uri (const char *uri)
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

  decoded = g_decoded_uri_new ();
  
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

      userinfo_end = memchr (authority_start, '@', authority_end - authority_start);
      if (userinfo_end)
	{
	  userinfo_start = authority_start;
	  decoded->userinfo = g_uri_unescape_segment (userinfo_start, userinfo_end, NULL);
	  if (decoded->userinfo == NULL)
	    {
	      g_decoded_uri_free (decoded);
	      return NULL;
	    }
	  host_start = userinfo_end + 1;
	}
      else
	host_start = authority_start;

      port_start = memchr (host_start, ':', authority_end - host_start);
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

      decoded->host = g_strndup (host_start, host_end - host_start);

      hier_part_start = authority_end;
    }

  decoded->path = g_uri_unescape_segment (hier_part_start, hier_part_end, "/");

  if (decoded->path == NULL)
    {
      g_decoded_uri_free (decoded);
      return NULL;
    }
  
  return decoded;
}

char *
g_encode_uri (GDecodedUri *decoded, gboolean allow_utf8)
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
      
      g_string_append (uri, decoded->host);
      
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
