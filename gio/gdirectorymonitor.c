#include <config.h>
#include <string.h>

#include "gdirectorymonitor.h"
#include "gvfs-marshal.h"
#include "gfile.h"
#include "gvfs.h"

static void g_directory_monitor_class_init (gpointer g_class, gpointer class_data);
static void g_directory_monitor_base_init (gpointer g_class);

GType
g_directory_monitor_get_type (void)
{
  static GType directory_monitor_type = 0;
  
  if (! directory_monitor_type)
    {
      static const GTypeInfo directory_monitor_info =
	{
	  sizeof (GDirectoryMonitorIface), /* class_size */
	  g_directory_monitor_base_init,   /* base_init */
	  NULL,				 /* base_finalize */
	  g_directory_monitor_class_init,  /* class_init */
	  NULL,				 /* class_finalize */
	  NULL,				 /* class_data */
	  0,
	  0,				 /* n_preallocs */
	  NULL
	};
      
      directory_monitor_type = g_type_register_static (G_TYPE_INTERFACE, I_("GDirectoryMonitor"), &directory_monitor_info, 0);
      g_type_interface_add_prerequisite (directory_monitor_type, G_TYPE_OBJECT);
    }
  return directory_monitor_type;
}

static void
g_directory_monitor_class_init (gpointer g_class,
				gpointer class_data)
{
}

static void
g_directory_monitor_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;
  
  if (! initialized)
    {
      g_signal_new (I_("change"),
		    G_TYPE_DIRECTORY_MONITOR,
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (GDirectoryMonitorIface, change),
		    NULL, NULL,
		    _gvfs_marshal_VOID__OBJECT_OBJECT_INT,
		    G_TYPE_NONE,3,
		    G_TYPE_FILE,
		    G_TYPE_FILE,
		    G_TYPE_INT);
      initialized = TRUE;
    }
}

gboolean
g_directory_monitor_cancel (GDirectoryMonitor* monitor)
{
  GDirectoryMonitorIface* iface;
  
  iface = G_DIRECTORY_MONITOR_GET_IFACE (monitor);
  
  return (* iface->cancel) (monitor);
}
