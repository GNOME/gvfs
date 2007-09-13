#include <config.h>
#include "gseekable.h"
#include <glib/gi18n-lib.h>

static void g_seekable_base_init (gpointer g_class);


GType
g_seekable_get_type (void)
{
  static GType seekable_type = 0;

  if (! seekable_type)
    {
      static const GTypeInfo seekable_info =
      {
        sizeof (GSeekableIface), /* class_size */
	g_seekable_base_init,   /* base_init */
	NULL,		/* base_finalize */
	NULL,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      seekable_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GSeekable"),
				&seekable_info, 0);

      g_type_interface_add_prerequisite (seekable_type, G_TYPE_OBJECT);
    }

  return seekable_type;
}

static void
g_seekable_base_init (gpointer g_class)
{
}


goffset
g_seekable_tell (GSeekable *seekable)
{
  GSeekableIface *iface;

  iface = G_SEEKABLE_GET_IFACE (seekable);

  return (* iface->tell) (seekable);
}

gboolean
g_seekable_can_seek (GSeekable *seekable)
{
  GSeekableIface *iface;

  iface = G_SEEKABLE_GET_IFACE (seekable);

  return (* iface->can_seek) (seekable);
}

gboolean
g_seekable_seek (GSeekable     *seekable,
		 goffset        offset,
		 GSeekType      type,
		 GCancellable  *cancellable,
		 GError       **error)
{
  GSeekableIface *iface;

  iface = G_SEEKABLE_GET_IFACE (seekable);

  return (* iface->seek) (seekable, offset, type, cancellable, error);
}

gboolean
g_seekable_can_truncate (GSeekable *seekable)
{
  GSeekableIface *iface;

  iface = G_SEEKABLE_GET_IFACE (seekable);

  return (* iface->can_truncate) (seekable);
}

gboolean
g_seekable_truncate (GSeekable     *seekable,
		     goffset        offset,
		     GCancellable  *cancellable,
		     GError       **error)
{
  GSeekableIface *iface;

  iface = G_SEEKABLE_GET_IFACE (seekable);

  return (* iface->truncate) (seekable, offset, cancellable, error);
}

