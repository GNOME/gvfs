#ifndef __G_VFS_TYPES_H__
#define __G_VFS_TYPES_H__

#include <glib.h>

G_BEGIN_DECLS

#define I_(string) g_intern_static_string (string)

#define G_MAXSSIZE G_MAXLONG

typedef gint64 goffset;

G_END_DECLS

#endif /* __G_VFS_TYPES_H__ */
