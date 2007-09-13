#ifndef __G_VFS_URI_UTILS_H__
#define __G_VFS_URI_UTILS_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *scheme;
  char *userinfo;
  char *host;
  int port;
  char *path;
  char *query;
  char *fragment;
} GDecodedUri;

char *       _g_encode_uri       (GDecodedUri *decoded,
				  gboolean     only_base);
void         _g_decoded_uri_free (GDecodedUri *decoded);
GDecodedUri *_g_decode_uri       (const char  *uri);

G_END_DECLS

#endif /* __G_VFS_URI_UTILS_H__ */
