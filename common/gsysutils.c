/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#if defined(HAVE_SYS_PARAM_H)
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#include <gio/gio.h>

#include <glib/gi18n-lib.h>

#include "gsysutils.h"

int
_g_socket_send_fd (int connection_fd, 
		   int fd)
{
  struct msghdr msg;
  struct iovec vec[1];
  char buf[1] = {'x'};
  char ccmsg[CMSG_SPACE (sizeof (fd))];
  struct cmsghdr *cmsg;
  int ret;
  
  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  vec[0].iov_base = buf;
  vec[0].iov_len = 1;
  msg.msg_iov = vec;
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
  struct iovec iov[1];
  char buf[1];
  int rv;
  char ccmsg[CMSG_SPACE (sizeof(int))];
  struct cmsghdr *cmsg;

  iov[0].iov_base = buf;
  iov[0].iov_len = 1;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
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
  if (cmsg == NULL)
    return -1;
  
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
      int errsv = errno;

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error creating socket: %s"),
		   g_strerror (errsv));
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
      int errsv = errno;

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error connecting to socket: %s"),
		   g_strerror (errsv));
      close (fd);
      return -1;
    }

  return fd;
}
