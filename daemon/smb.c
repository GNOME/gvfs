#include <config.h>

#include <glib.h>
#include "daemon-main.h"
#include "gvfsbackendsmb.h"

int
main (int argc, char *argv[])
{
  daemon_init ();
  daemon_main (argc, argv,
	       "smb-share",
	       NULL,
	       "smb-share", G_TYPE_VFS_BACKEND_SMB,
	       NULL);
  
  return 0;
}
