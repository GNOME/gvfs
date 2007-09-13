#ifndef __G_VFS_URI_UTILS_H__
#define __G_VFS_URI_UTILS_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *scheme;
  char *userinfo;
  char *host;
  int port; /* -1 => not in uri */
  char *path;
  char *query;
  char *fragment;
} GDecodedUri;

char *       g_uri_unescape_string       (const gchar *escaped_string,
					  const gchar *escaped_string_end,
					  const gchar *illegal_characters);
void         g_string_append_uri_encoded (GString     *string,
					  const char  *encoded,
					  const char  *reserved_chars_allowed,
					  gboolean     allow_utf8);

char *       g_encode_uri                (GDecodedUri *decoded,
					  gboolean     allow_utf8);
void         g_decoded_uri_free          (GDecodedUri *decoded);
GDecodedUri *g_decode_uri                (const char  *uri);
GDecodedUri *g_decoded_uri_new           (void);



G_END_DECLS

#endif /* __G_VFS_URI_UTILS_H__ */
