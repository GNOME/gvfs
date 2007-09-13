#include <config.h>

#include <glib.h>
#include "daemon-main.h"
#include "gvfsbackendtest.h"

int
main (int argc, char *argv[])
{
  daemon_init ();
  daemon_main (argc, argv,
	       "test",
	       NULL,
	       "test", G_TYPE_VFS_BACKEND_TEST,
	       NULL);
  
  return 0;
}
