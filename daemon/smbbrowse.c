#include <config.h>

#include <glib.h>
#include "daemon-main.h"
#include "gvfsbackendsmbbrowse.h"

int
main (int argc, char *argv[])
{
  daemon_init ();
  daemon_main (argc, argv,
	       NULL,
	       "org.gtk.vfs.mountpoint.smb_browse",
	       "smb-network", G_TYPE_VFS_BACKEND_SMB_BROWSE,
	       "smb-server", G_TYPE_VFS_BACKEND_SMB_BROWSE,
	       NULL);
	       
  return 0;
}
