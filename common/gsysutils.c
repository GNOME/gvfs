#include <config.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <gio/gioerror.h>

#include <glib/gi18n-lib.h>

#include "gsysutils.h"

int
_g_socket_send_fd (int connection_fd, 
		   int fd)
{
  struct msghdr msg;
  struct iovec vec;
  char buf[1] = {'x'};
  char ccmsg[CMSG_SPACE (sizeof (fd))];
  struct cmsghdr *cmsg;
  int ret;
  
  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  vec.iov_base = buf;
  vec.iov_len = 1;
  msg.msg_iov = &vec;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof(fd));
  *(int*)CMSG_DATA (cmsg) = fd;
  msg.msg_controllen = cmsg->cmsg_len;
  msg.msg_flags = 0;

  ret = sendmsg (connection_fd, &msg, 0);
  return ret;
}

/* receive a file descriptor over file descriptor fd */
int 
_g_socket_receive_fd (int socket_fd)
{
  struct msghdr msg;
  struct iovec iov;
  char buf[1];
  int rv;
  char ccmsg[CMSG_SPACE (sizeof(int))];
  struct cmsghdr *cmsg;

  iov.iov_base = buf;
  iov.iov_len = 1;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  
  rv = recvmsg (socket_fd, &msg, 0);
  if (rv == -1) 
    {
      perror ("recvmsg");
      return -1;
    }

  cmsg = CMSG_FIRSTHDR (&msg);
  if (!cmsg->cmsg_type == SCM_RIGHTS) {
    g_warning("got control message of unknown type %d", 
	      cmsg->cmsg_type);
    return -1;
  }

  return *(int*)CMSG_DATA(cmsg);
}

int
_g_socket_connect (const char *address,
		   GError **error)
{
  int fd;
  const char *path;
  size_t path_len;
  struct sockaddr_un addr;
  gboolean abstract;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error creating socket: %s"),
		   g_strerror (errno));
      return -1;
    }
  
  if (g_str_has_prefix (address, "unix:abstract="))
    {
      path = address + strlen ("unix:abstract=");
      abstract = TRUE;
    }
  else
    {
      path = address + strlen ("unix:path=");
      abstract = FALSE;
    }
    
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  path_len = strlen (path);

  if (abstract)
    {
      addr.sun_path[0] = '\0'; /* this is what says "use abstract" */
      path_len++; /* Account for the extra nul byte added to the start of sun_path */

      strncpy (&addr.sun_path[1], path, path_len);
    }
  else
    {
      strncpy (addr.sun_path, path, path_len);
    }
  
  if (connect (fd, (struct sockaddr*) &addr, G_STRUCT_OFFSET (struct sockaddr_un, sun_path) + path_len) < 0)
    {      
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error connecting to socket: %s"),
		   g_strerror (errno));
      close (fd);
      return -1;
    }

  return fd;
}
