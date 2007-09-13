#include <config.h>
#include "gvfsuriutils.h"
#include <string.h>
#include <stdlib.h>

static int
unescape_character (const char *scanner)
{
  int first_digit;
  int second_digit;
  
  first_digit = g_ascii_xdigit_value (*scanner++);
  if (first_digit < 0)
    return -1;

  second_digit = g_ascii_xdigit_value (*scanner++);
  if (second_digit < 0)
    return -1;

  return (first_digit << 4) | second_digit;
}

static char *
unescape_string (const gchar *escaped_string,
		 const gchar *escaped_string_end,
		 const gchar *illegal_characters)
{
  const gchar *in;
  gchar *out, *result;
  gint character;
  
  if (escaped_string == NULL)
    return NULL;

  result = g_malloc (escaped_string_end - escaped_string + 1);
	
  out = result;
  for (in = escaped_string; in < escaped_string_end; in++) {
    character = *in;
    if (*in == '%') {
      in++;
      if (escaped_string_end - in < 2)
	{
	  g_free (result);
	  return NULL;
	}
      
      character = unescape_character (in);
      
      /* Check for an illegal character. We consider '\0' illegal here. */
      if (character <= 0 ||
	  (illegal_characters != NULL &&
	   strchr (illegal_characters, (char)character) != NULL))
	{
	  g_free (result);
	  return NULL;
	}
      in++; /* The other char will be eaten in the loop header */
    }
    *out++ = (char)character;
  }
  
  *out = '\0';
  g_assert (out - result <= strlen (escaped_string));
  return result;
}

void
_g_decoded_uri_free (GDecodedUri *decoded)
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
_g_decode_uri (const char *uri)
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

  decoded = g_new0 (GDecodedUri, 1);
  decoded->port = -1;
  
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
	  decoded->userinfo = unescape_string (userinfo_start, userinfo_end, NULL);
	  if (decoded->userinfo == NULL)
	    {
	      _g_decoded_uri_free (decoded);
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

  decoded->path = unescape_string (hier_part_start, hier_part_end, "/");

  if (decoded->path == NULL)
    {
      _g_decoded_uri_free (decoded);
      return NULL;
    }
  
  return decoded;
}

#define SUB_DELIM_CHARS  "!$&'()*+,;="

static gboolean
is_valid (char c, const char *reserved_chars_allowed)
{
  if (g_ascii_isalnum (c) ||
      c == '-' ||
      c == '.' ||
      c == '_' ||
      c == '~')
    return TRUE;

  if (reserved_chars_allowed &&
      strchr (reserved_chars_allowed, c) != NULL)
    return TRUE;
  
  return FALSE;
}

static void
g_string_append_encoded (GString *string, const char *encoded,
			 const char *reserved_chars_allowed)
{
  char c;
  static const gchar hex[16] = "0123456789ABCDEF";
  
  while ((c = *encoded++) != 0)
    {
      if (is_valid (c, reserved_chars_allowed))
	g_string_append_c (string, c);
      else
	{
	  g_string_append_c (string, '%');
	  g_string_append_c (string, hex[((guchar)c) >> 4]);
	  g_string_append_c (string, hex[((guchar)c) & 0xf]);
	}
    }
}

char *
_g_encode_uri (GDecodedUri *decoded, gboolean only_base)
{
  GString *uri;

  uri = g_string_new (NULL);

  g_string_append (uri, decoded->scheme);
  g_string_append_c (uri, ':');

  if (decoded->host != NULL)
    {
      g_string_append (uri, "//");
      
      if (decoded->userinfo)
	{
	  /* userinfo    = *( unreserved / pct-encoded / sub-delims / ":" ) */
	  g_string_append_encoded (uri, decoded->userinfo, SUB_DELIM_CHARS ":");
	  g_string_append_c (uri, '@');
	}
      
      g_string_append (uri, decoded->host);
      
      if (decoded->port != -1)
	{
	  g_string_append_c (uri, ':');
	  g_string_append_printf (uri, "%d", decoded->port);
	}
    }

  if (only_base)
    return g_string_free (uri, FALSE);
    
  
  g_string_append_encoded (uri, decoded->path, SUB_DELIM_CHARS ":@/");
  
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
