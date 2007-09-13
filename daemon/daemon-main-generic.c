#include <config.h>

#include <glib.h>
#include "daemon-main.h"
#include G_STRINGIFY(BACKEND_HEADER)

int
main (int argc, char *argv[])
{
  daemon_init ();
  daemon_main (argc, argv,
#ifdef MAX_JOB_THREADS
	       MAX_JOB_THREADS,
#else
	       -1, 
#endif
#ifdef DEFAULT_BACKEND_TYPE
	       G_STRINGIFY(DEFAULT_BACKEND_TYPE),
#else
	       NULL,
#endif
#ifdef MOUNTABLE_DBUS_NAME
	       G_STRINGIFY(MOUNTABLE_DBUS_NAME),
#else
	       NULL,
#endif
	       BACKEND_TYPES
	       NULL);
	       
  return 0;
}
