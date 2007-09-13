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

char *       _g_encode_uri       (GDecodedUri *decoded,
				  gboolean     allow_utf8);
void         _g_decoded_uri_free (GDecodedUri *decoded);
GDecodedUri *_g_decode_uri       (const char  *uri);
GDecodedUri *_g_decoded_uri_new  (void);


G_END_DECLS

#endif /* __G_VFS_URI_UTILS_H__ */
