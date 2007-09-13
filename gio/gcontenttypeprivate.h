#ifndef __G_CONTENT_TYPE_PRIVATE_H__
#define __G_CONTENT_TYPE_PRIVATE_H__

#include "gcontenttype.h"

G_BEGIN_DECLS

gsize  _g_unix_content_type_get_sniff_len (void);
char * _g_unix_content_type_unalias       (const char *type);
char **_g_unix_content_type_get_parents   (const char *type);

G_END_DECLS

#endif /* __G_CONTENT_TYPE_PRIVATE_H__ */
