#ifndef __G_SEEKABLE_H__
#define __G_SEEKABLE_H__

#include <glib-object.h>
#include <gvfs/gvfstypes.h>

G_BEGIN_DECLS

#define G_TYPE_SEEKABLE            (g_seekable_get_type ())
#define G_SEEKABLE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_SEEKABLE, GSeekable))
#define G_IS_SEEKABLE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_SEEKABLE))
#define G_SEEKABLE_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_SEEKABLE, GSeekableIface))

typedef struct _GSeekable        GSeekable;
typedef struct _GSeekableIface   GSeekableIface;

struct _GSeekableIface
{
  GTypeInterface g_iface;

  /* Virtual Table */
  
  goffset     (* tell)	         (GSeekable *seekable);
  goffset     (* seek)	         (GSeekable *seekable,
				  goffset    offset,
				  GSeekType  type,
				  GError   **err);
  gboolean    (* can_truncate)   (GSeekable *seekable);
  goffset     (* truncate)       (GSeekable *seekable,
				  goffset    offset,
				  GError    **err);

}

goffset  g_seekable_tell     (GSeekable  *seekable);
goffset  g_seekable_seek     (GSeekable *seekable,
			      goffset    offset,
			      GSeekType  type,
			      GError   **err);

G_END_DECLS


#endif /* __G_SEEKABLE_H__ */
