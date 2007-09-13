#include <config.h>

#include "gthemedicon.h"

static void g_themed_icon_icon_iface_init (GIconIface       *iface);

struct _GThemedIcon
{
  GObject parent_instance;
  
  char **names;
};

G_DEFINE_TYPE_WITH_CODE (GThemedIcon, g_themed_icon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_ICON,
						g_themed_icon_icon_iface_init))
  
static void
g_themed_icon_finalize (GObject *object)
{
  GThemedIcon *themed;

  themed = G_THEMED_ICON (object);

  g_strfreev (themed->names);
  
  if (G_OBJECT_CLASS (g_themed_icon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_themed_icon_parent_class)->finalize) (object);
}

static void
g_themed_icon_class_init (GThemedIconClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_themed_icon_finalize;
}

static void
g_themed_icon_init (GThemedIcon *themed)
{
}

GIcon *
g_themed_icon_new (const char *iconname)
{
  GThemedIcon *themed;

  themed = g_object_new (G_TYPE_THEMED_ICON, NULL);
  themed->names = g_new (char *, 2);
  themed->names[0] = g_strdup (iconname);
  themed->names[1] = NULL;
  
  return G_ICON (themed);
}

GIcon *
g_themed_icon_new_from_names (char **iconnames, int len)
{
  GThemedIcon *themed;
  int i;
  
  themed = g_object_new (G_TYPE_THEMED_ICON, NULL);
  if (len == -1)
    themed->names = g_strdupv (iconnames);
  else
    {
      themed->names = g_new (char *, len + 1);
      for (i = 0; i < len; i++)
	themed->names[i] = g_strdup (iconnames[i]);
    }
  
  
  return G_ICON (themed);
}

char **
g_themed_icon_get_names (GThemedIcon *icon)
{
  return icon->names;
}

static guint
g_themed_icon_hash (GIcon *icon)
{
  GThemedIcon *themed = G_THEMED_ICON (icon);
  guint hash;
  int i;

  hash = 0;

  for (i = 0; themed->names[i] != NULL; i++)
    hash ^= g_str_hash (themed->names[i]);
  
  return hash;
}

static gboolean
g_themed_icon_equal (GIcon *icon1,
		    GIcon *icon2)
{
  GThemedIcon *themed1 = G_THEMED_ICON (icon1);
  GThemedIcon *themed2 = G_THEMED_ICON (icon2);
  int i;

  for (i = 0; themed1->names[i] != NULL && themed2->names[i] != NULL; i++)
    {
      if (!g_str_equal (themed1->names[i], themed2->names[i]))
	return FALSE;
    }

  return themed1->names[i] == NULL && themed2->names[i] == NULL;
}


static void
g_themed_icon_icon_iface_init (GIconIface *iface)
{
  iface->hash = g_themed_icon_hash;
  iface->equal = g_themed_icon_equal;
}
