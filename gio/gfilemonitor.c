#include <config.h>
#include <string.h>

#include "gfilemonitor.h"
#include "gvfs-marshal.h"
#include "gfile.h"
#include "gvfs.h"

static void g_file_monitor_class_init (gpointer g_class,
				       gpointer class_data);
static void g_file_monitor_base_init  (gpointer g_class);


GType
g_file_monitor_get_type (void)
{
  static GType file_monitor_type = 0;
  
  if (! file_monitor_type)
    {
      static const GTypeInfo file_monitor_info =
	{
	  sizeof (GFileMonitorIface), /* class_size */
	  g_file_monitor_base_init,   /* base_init */
	  NULL,			    /* base_finalize */
	  g_file_monitor_class_init,  /* class_init */
	  NULL,			    /* class_finalize */
	  NULL,			    /* class_data */
	  0,
	  0,			    /* n_preallocs */
	  NULL
	};
      
      file_monitor_type = g_type_register_static (G_TYPE_INTERFACE, I_("GFileMonitor"), &file_monitor_info, 0);
      g_type_interface_add_prerequisite (file_monitor_type, G_TYPE_OBJECT);
    }
  return file_monitor_type;
}

static void
g_file_monitor_class_init (gpointer g_class,
			   gpointer class_data)
{
}

static void
g_file_monitor_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;
  
  if (! initialized)
    {
      g_signal_new (I_("changed"),
		    G_TYPE_FILE_MONITOR,
		    G_SIGNAL_RUN_LAST,
		    G_STRUCT_OFFSET (GFileMonitorIface, changed),
		    NULL, NULL,
		    _gvfs_marshal_VOID__OBJECT_INT,
		    G_TYPE_NONE,2,
		    G_TYPE_FILE,
		    G_TYPE_INT);
      initialized = TRUE;
    }
}

gboolean
g_file_monitor_cancel (GFileMonitor* monitor)
{
  GFileMonitorIface* iface;
  
  iface = G_FILE_MONITOR_GET_IFACE (monitor);
  
  return (* iface->cancel) (monitor);
}
