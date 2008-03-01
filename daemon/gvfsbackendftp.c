/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjmain Otte <otte@gnome.org>
 */


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include <libsoup/soup.h>

#include "gvfsbackendftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"
#include "gvfskeyring.h"

#include "ParseFTPList.h"

#if 1
#define DEBUG g_print
#else
#define DEBUG(...)
#endif

/*
 * about filename interpretation in the ftp backend
 *
 * As GVfs composes paths using a slash character, we cannot allow a slash as 
 * part of a basename. Other critical characters are \r \n and sometimes the 
 * space. We therefore g_uri_escape_string() filenames by default and concatenate 
 * paths using slashes. This should make GVfs happy.
 *
 * Luckily, TVFS (see RFC 3xxx for details) is a specification that does exactly 
 * what we want. It disallows slashes, \r and \n in filenames, so we can happily 
 * use it without the need to escape. We also can operate on full paths as our 
 * paths exactly match those of a TVFS-using FTP server.
 */
typedef enum {
  FTP_FEATURE_MDTM = (1 << 0),
  FTP_FEATURE_SIZE = (1 << 1),
  FTP_FEATURE_TVFS = (1 << 2)
} FtpFeatures;

struct _GVfsBackendFtp
{
  GVfsBackend		backend;

  SoupAddress *		addr;
  char *		user;
  char *		password;

  /* connection collection */
  GQueue *		queue;
  GMutex *		mutex;
  GCond *		cond;
};

G_DEFINE_TYPE (GVfsBackendFtp, g_vfs_backend_ftp, G_VFS_TYPE_BACKEND)

#define STATUS_GROUP(status) ((status) / 100)

/*** FTP CONNECTION ***/

typedef struct _FtpConnection FtpConnection;

struct _FtpConnection
{
  GCancellable *	cancellable;

  FtpFeatures		features;

  SoupSocket *		commands;
  gchar			read_buffer[256];
  gsize			read_bytes;

  SoupSocket *		data;
};

static void
ftp_connection_free (FtpConnection *conn)
{
  if (conn->commands)
    g_object_unref (conn->commands);
  if (conn->data)
    g_object_unref (conn->data);

  g_slice_free (FtpConnection, conn);
}

/**
 * ftp_error_set_from_response:
 * @error: pointer to an error to be set or %NULL
 * @response: an FTP response code to use as the error message
 *
 * Sets an error based on an FTP response code.
 **/
static void
ftp_error_set_from_response (GError **error, guint response)
{
  const char *msg;
  int code;

  /* Please keep this list ordered by response code,
   * but group responses with the same message. */
  switch (response)
    {
      case 332: /* Need account for login. */
      case 532: /* Need account for storing files. */
	/* FIXME: implement a sane way to handle accounts. */
	code = G_IO_ERROR_NOT_SUPPORTED;
	msg = _("Accounts are unsupported");
	break;
      case 421: /* Service not available, closing control connection. */
	code = G_IO_ERROR_FAILED;
	msg = _("Host closed connection");
	break;
      case 425: /* Can't open data connection. */
	code = G_IO_ERROR_CLOSED;
	msg = _("Cannot open data connection. Maybe your firewall prevents this?");
	break;
      case 426: /* Connection closed; transfer aborted. */
	code = G_IO_ERROR_CLOSED;
	msg = _("Data connection closed");
	break;
      case 450: /* Requested file action not taken. File unavailable (e.g., file busy). */
      case 550: /* Requested action not taken. File unavailable (e.g., file not found, no access). */
	/* FIXME: This is a lot of different errors */
	code = G_IO_ERROR_NOT_FOUND;
	msg = _("File unavailable");
	break;
      case 451: /* Requested action aborted: local error in processing. */
	code = G_IO_ERROR_FAILED;
	msg = _("Operation failed");
	break;
      case 452: /* Requested action not taken. Insufficient storage space in system. */
      case 552:
	code = G_IO_ERROR_NO_SPACE;
	msg = _("No space left on server");
	break;
      case 500: /* Syntax error, command unrecognized. */
      case 501: /* Syntax error in parameters or arguments. */
      case 502: /* Command not implemented. */
      case 503: /* Bad sequence of commands. */
      case 504: /* Command not implemented for that parameter. */
	code = G_IO_ERROR_NOT_SUPPORTED;
	msg = _("Operation unsupported");
	break;
      case 530: /* Not logged in. */
	code = G_IO_ERROR_PERMISSION_DENIED;
	msg = _("Permission denied");
	break;
      case 551: /* Requested action aborted: page type unknown. */
	code = G_IO_ERROR_FAILED;
	msg = _("Page type unknown");
	break;
      case 553: /* Requested action not taken. File name not allowed. */
	code = G_IO_ERROR_INVALID_FILENAME;
	msg = _("Invalid filename");
	break;
      default:
	code = G_IO_ERROR_FAILED;
	msg = _("Invalid reply from server");
	break;
    }

  DEBUG ("error: %s\n", msg);
  g_set_error (error, G_IO_ERROR, code, msg);
}

/**
 * ResponseFlags:
 * RESPONSE_PASS_100: Don't treat 1XX responses, but return them
 * RESPONSE_PASS_300: Don't treat 3XX responses, but return them
 * RESPONSE_PASS_400: Don't treat 4XX responses, but return them
 * RESPONSE_PASS_500: Don't treat 5XX responses, but return them
 * RESPONSE_FAIL_200: Fail on a 2XX response
 */

typedef enum {
  RESPONSE_PASS_100 = (1 << 0),
  RESPONSE_PASS_300 = (1 << 1),
  RESPONSE_PASS_400 = (1 << 2),
  RESPONSE_PASS_500 = (1 << 3),
  RESPONSE_FAIL_200 = (1 << 4)
} ResponseFlags;

/**
 * ftp_connection_receive:
 * @conn: connection to receive from
 * @flags: flags for handling the response
 * @error: pointer to error message
 *
 * Reads a command and stores it in @conn->read_buffer. The read buffer will be
 * null-terminated and contain @conn->read_bytes bytes. Afterwards, the response
 * will be parsed and processed according to @flags. By default, all responses
 * but 2xx will cause an error.
 *
 * Returns: 0 on error, the ftp code otherwise
 **/
static guint
ftp_connection_receive (FtpConnection *conn,
			ResponseFlags  flags,
			GError **      error)
{
  SoupSocketIOStatus status;
  gsize n_bytes;
  gboolean got_boundary;
  char *last_line;
  enum {
    FIRST_LINE,
    MULTILINE,
    DONE
  } reply_state = FIRST_LINE;
  guint response = 0;
  gsize bytes_left;

  conn->read_bytes = 0;
  bytes_left = sizeof (conn->read_buffer) - conn->read_bytes - 1;
  while (reply_state != DONE && bytes_left >= 6)
    {
      last_line = conn->read_buffer + conn->read_bytes;
      status = soup_socket_read_until (conn->commands,
				       last_line,
				       bytes_left,
				       "\r\n",
				       2,
				       &n_bytes,
				       &got_boundary,
				       conn->cancellable,
				       error);
      switch (status)
	{
	  case SOUP_SOCKET_OK:
	  case SOUP_SOCKET_EOF:
	    if (got_boundary)
	      break;
	    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			 _("Invalid reply"));
	    /* fall through */
	  case SOUP_SOCKET_ERROR:
	    conn->read_buffer[conn->read_bytes] = 0;
	    return 0;
	  case SOUP_SOCKET_WOULD_BLOCK:
	  default:
	    g_assert_not_reached ();
	    break;
	}

      bytes_left -= n_bytes;
      conn->read_bytes += n_bytes;
      conn->read_buffer[conn->read_bytes] = 0;
      DEBUG ("<-- %s", last_line);

      if (reply_state == FIRST_LINE)
	{
	  if (n_bytes < 4 ||
	      last_line[0] <= '0' || last_line[0] > '5' ||
	      last_line[1] < '0' || last_line[1] > '9' ||
	      last_line[2] < '0' || last_line[2] > '9')
	    {
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid reply"));
	      return 0;
	    }
	  response = 100 * (last_line[0] - '0') +
		      10 * (last_line[1] - '0') +
		 	   (last_line[2] - '0');
	  if (last_line[3] == ' ')
	    reply_state = DONE;
	  else if (last_line[3] == '-')
	    reply_state = MULTILINE;
	  else
	    {
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid reply"));
	      return 0;
	    }
	}
      else
	{
	  if (n_bytes >= 4 &&
	      memcmp (conn->read_buffer, last_line, 3) == 0 &&
	      last_line[3] == ' ')
	    reply_state = DONE;
	}
    }

  if (reply_state != DONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid reply"));
      return 0;
    }

  switch (STATUS_GROUP (response))
    {
      case 0:
	return 0;
      case 1:
	if (flags & RESPONSE_PASS_100)
	  break;
	ftp_error_set_from_response (error, response);
	return 0;
      case 2:
	if (flags & RESPONSE_FAIL_200)
	  {
	    ftp_error_set_from_response (error, response);
	    return 0;
	  }
	break;
      case 3:
	if (flags & RESPONSE_PASS_300)
	  break;
	ftp_error_set_from_response (error, response);
	return 0;
      case 4:
	if (flags & RESPONSE_PASS_400)
	  break;
	ftp_error_set_from_response (error, response);
	return 0;
	break;
      case 5:
	if (flags & RESPONSE_PASS_500)
	  break;
	ftp_error_set_from_response (error, response);
	return 0;
      default:
	g_assert_not_reached ();
	break;
    }

  return response;
}

/**
 * ftp_connection_send:
 * @conn: the connection to send to
 * @flags: #ResponseFlags to use
 * @error: pointer to take an error
 * @format: format string to construct command from 
 *          (without trailing \r\n)
 * @...: arguments to format string
 *
 * Takes a command, waits for an answer and parses it. Without any @flags, FTP 
 * codes other than 2xx cause an error. The last read ftp command will be put 
 * into @conn->read_buffer.
 *
 * Returns: 0 on error or the receied FTP code otherwise.
 *     
 **/
static guint
ftp_connection_sendv (FtpConnection *conn,
		      ResponseFlags  flags,
		      GError **	     error,
		      const char *   format,
		      va_list	     varargs)
{
  GString *command;
  SoupSocketIOStatus status;
  gsize n_bytes;
  guint response;

  command = g_string_new ("");
  g_string_append_vprintf (command, format, varargs);
  DEBUG ("--> %s\n", command->str);
  g_string_append (command, "\r\n");
  status = soup_socket_write (conn->commands,
			      command->str,
			      command->len,
			      &n_bytes,
			      conn->cancellable,
			      error);
  switch (status)
    {
      case SOUP_SOCKET_OK:
      case SOUP_SOCKET_EOF:
	if (n_bytes == command->len)
	  break;
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
	    _("broken transmission"));
	/* fall through */
      case SOUP_SOCKET_ERROR:
	g_string_free (command, TRUE);
	return 0;
      case SOUP_SOCKET_WOULD_BLOCK:
      default:
	g_assert_not_reached ();
    }
  g_string_free (command, TRUE);

  response = ftp_connection_receive (conn, flags, error);
  return response;
}

static guint
ftp_connection_send (FtpConnection *conn,
		     ResponseFlags  flags,
		     GError **	    error,
		     const char *   format,
		     ...) G_GNUC_PRINTF (4, 5);
static guint
ftp_connection_send (FtpConnection *conn,
		     ResponseFlags  flags,
		     GError **	    error,
		     const char *   format,
		     ...)
{
  va_list varargs;
  guint response;

  va_start (varargs, format);
  response = ftp_connection_sendv (conn,
				   flags,
				   error,
				   format,
				   varargs);
  va_end (varargs);
  return response;
}

static void
ftp_connection_parse_features (FtpConnection *conn)
{
  struct {
    const char *	name;		/* name of feature */
    FtpFeatures		enable;		/* flags to enable with this feature */
  } features[] = {
    { "MDTM", FTP_FEATURE_MDTM },
    { "SIZE", FTP_FEATURE_SIZE },
    { "TVFS", FTP_FEATURE_TVFS }
  };
  char **supported;
  guint i, j;

  supported = g_strsplit (conn->read_buffer, "\r\n", -1);

  for (i = 1; supported[i]; i++)
    {
      const char *feature = supported[i];
      if (feature[0] != ' ')
	continue;
      feature++;
      for (j = 0; j < G_N_ELEMENTS (features); j++)
	{
	  if (g_ascii_strcasecmp (feature, features[j].name) == 0)
	    {
	      DEBUG ("feature %s supported\n", features[j].name);
	      conn->features |= features[j].enable;
	    }
	}
    }
}

static FtpConnection *
ftp_connection_create (SoupAddress * addr, 
		       GCancellable *cancellable,
		       GError **     error)
{
  FtpConnection *conn;
  guint status;

  conn = g_slice_new0 (FtpConnection);
  conn->cancellable = cancellable;
  conn->commands = soup_socket_new ("non-blocking", FALSE,
                                    "remote-address", addr,
				    NULL);
  status = soup_socket_connect_sync (conn->commands, cancellable);
  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* FIXME: better error messages depending on status please */
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_HOST_NOT_FOUND,
		   _("Could not connect to host"));
      goto fail;
    }

  status = ftp_connection_receive (conn, 0, error);
  if (status == 0)
    goto fail;
  
  return conn;

fail:
  ftp_connection_free (conn);
  return NULL;
}

static guint
ftp_connection_login (FtpConnection *conn,
		      const char *   username,
		      const char *   password,
		      GError **	     error)
{
  guint status;

  status = ftp_connection_send (conn, RESPONSE_PASS_300, error,
                                "USER %s", username);
  
  if (STATUS_GROUP (status) == 3)
    status = ftp_connection_send (conn, 0, error,
				  "PASS %s", password);

  return status;
}

static gboolean
ftp_connection_use (FtpConnection *conn,
		    GError **	   error)
{
  guint status;

  /* only binary transfers please */
  status = ftp_connection_send (conn, 0, error, "TYPE I");
  if (status == 0)
    return FALSE;

  /* check supported features */
  status = ftp_connection_send (conn, 0, NULL, "FEAT");
  if (status != 0)
    ftp_connection_parse_features (conn);

  return TRUE;
}

#if 0
static FtpConnection *
ftp_connection_new (SoupAddress * addr, 
                    GCancellable *cancellable,
		    const char *  username,
		    const char *  password,
		    GError **	  error)
{
  FtpConnection *conn;

  conn = ftp_connection_create (addr, cancellable, error);
  if (conn == NULL)
    return NULL;

  if (ftp_connection_login (conn, username, password, error) == 0 ||
      !ftp_connection_use (conn, error))
    {
      ftp_connection_free (conn);
      return NULL;
    }

  return conn;
}
#endif

static gboolean
ftp_connection_ensure_data_connection (FtpConnection *conn, 
                                       GError **      error)
{
  guint ip1, ip2, ip3, ip4, port1, port2;
  SoupAddress *addr;
  const char *s;
  char *ip;
  guint status;

  /* only binary transfers please */
  status = ftp_connection_send (conn, 0, error, "PASV");
  if (status == 0)
    return FALSE;

  /* parse response and try to find the address to connect to.
   * This code does the sameas curl.
   */
  for (s = conn->read_buffer; *s; s++)
    {
      if (sscanf (s, "%u,%u,%u,%u,%u,%u", 
                 &ip1, &ip2, &ip3, &ip4, 
                 &port1, &port2) == 6)
       break;
    }
  if (*s == 0)
    {
      return FALSE;
    }
  ip = g_strdup_printf ("%u.%u.%u.%u", ip1, ip2, ip3, ip4);
  addr = soup_address_new (ip, port1 << 8 | port2);
  g_free (ip);

  conn->data = soup_socket_new ("non-blocking", FALSE,
				"remote-address", addr,
				NULL);
  g_object_unref (addr);
  status = soup_socket_connect_sync (conn->data , conn->cancellable);
  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* FIXME: better error messages depending on status please */
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_HOST_NOT_FOUND,
		   _("Could not connect to host"));
      g_object_unref (conn->data);
      conn->data = NULL;
      return FALSE;
    }

  return TRUE;
}

static void
ftp_connection_close_data_connection (FtpConnection *conn)
{
  if (conn == NULL || conn->data == NULL)
    return;

  g_object_unref (conn->data);
  conn->data = NULL;
}

/*** FILE MAPPINGS ***/

/* FIXME: This most likely needs adaption to non-unix like directory structures.
 * There's at least the case of multiple roots (Netware) plus probably a shitload
 * of weird old file systems (starting with MS-DOS)
 * But we first need a way to detect that.
 */

/**
 * FtpFile:
 *
 * Byte string used to identify a file on the FTP server. It's typedef'ed to
 * make it easy to distinguish from GVfs paths.
 */

/* unsinged char is on purpose, so we get warnings when we misuse them */
typedef unsigned char FtpFile;

static FtpFile *
ftp_filename_from_gvfs_path (FtpConnection *conn, const char *pathname)
{
  return (FtpFile *) g_strdup (pathname);
}

static char *
ftp_filename_to_gvfs_path (FtpConnection *conn, const FtpFile *filename)
{
  return g_strdup ((const char *) filename);
}

/* Takes an FTP dirname and a basename (as used in RNTO or as result from LIST 
 * or similar) and gets the new ftp filename from it.
 *
 * Returns: the filename or %NULL if filename construction wasn't possible.
 */
/* let's hope we can live without a connection here, or we have to rewrite LIST */
static FtpFile *
ftp_filename_construct (FtpConnection *conn, const FtpFile *dirname, const char *basename)
{
  if (strpbrk (basename, "/\r\n"))
    return NULL;

  return (FtpFile *) g_strconcat ((char *) dirname, "/", basename, NULL);
}

/*** COMMON FUNCTIONS WITH SPECIAL HANDLING ***/

static gboolean
ftp_connection_cd (FtpConnection *conn, const FtpFile *file, GError **error)
{
  guint response = ftp_connection_send (conn,
					RESPONSE_PASS_500,
					error,
					"CWD %s", file);
  if (response == 5)
    {
      g_clear_error (error);
      g_set_error (error, 
	           G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
		   _("The file is not a directory"));
      response = 0;
    }
  else if (STATUS_GROUP (response) == 5)
    {
      ftp_error_set_from_response (error, response);
      response = 0;
    }

  return response != 0;
}

/*** BACKEND ***/

static void
g_vfs_backend_ftp_push_connection (GVfsBackendFtp *ftp, FtpConnection *conn)
{
  /* we allow conn == NULL to ease error cases */
  if (conn == NULL)
    return;

  conn->cancellable = NULL;

  g_mutex_lock (ftp->mutex);
  if (ftp->queue)
    {
      g_queue_push_tail (ftp->queue, conn);
      g_cond_signal (ftp->cond);
    }
  else
    ftp_connection_free (conn);
  g_mutex_unlock (ftp->mutex);
}

static void
do_broadcast (GCancellable *cancellable, GCond *cond)
{
  g_cond_broadcast (cond);
}

static FtpConnection *
g_vfs_backend_ftp_pop_connection (GVfsBackendFtp *ftp, 
                                  GCancellable *  cancellable,
				  GError **       error)
{
  FtpConnection *conn;

  g_mutex_lock (ftp->mutex);
  conn = ftp->queue ? g_queue_pop_head (ftp->queue) : NULL;
  if (conn == NULL && ftp->queue != NULL)
    {
      guint id = g_signal_connect (cancellable, 
				   "cancelled", 
				   G_CALLBACK (do_broadcast),
				   ftp->cond);
      while (conn == NULL && ftp->queue == NULL && !g_cancellable_is_cancelled (cancellable))
	{
	  g_cond_wait (ftp->cond, ftp->mutex);
	  conn = g_queue_pop_head (ftp->queue);
	}
      g_signal_handler_disconnect (cancellable, id);
    }
  g_mutex_unlock (ftp->mutex);

  if (conn == NULL)
    {
      /* FIXME: need different error on force-unmount? */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
	           _("Operation was cancelled"));
    }
  else
    conn->cancellable = cancellable;

  return conn;
}

static void
g_vfs_backend_ftp_finalize (GObject *object)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (object);

  if (ftp->addr)
    g_object_unref (ftp->addr);

  /* has been cleared on unmount */
  g_assert (ftp->queue == NULL);
  g_cond_free (ftp->cond);
  g_mutex_free (ftp->mutex);

  g_free (ftp->user);
  g_free (ftp->password);

  if (G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize) (object);
}

static void
g_vfs_backend_ftp_init (GVfsBackendFtp *ftp)
{
  ftp->mutex = g_mutex_new ();
  ftp->cond = g_cond_new ();
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  char *host;
  char *prompt = NULL;
  char *username;
  char *password;
  char *display_name;
  gboolean aborted;
  GError *error = NULL;
  GPasswordSave password_save = G_PASSWORD_SAVE_NEVER;
  guint port;

  port = soup_address_get_port (ftp->addr);
  /* FIXME: need to translate this? */
  if (port == 21)
    host = g_strdup (soup_address_get_name (ftp->addr));
  else
    host = g_strdup_printf ("%s:%u", 
	                    soup_address_get_name (ftp->addr),
	                    port);

  conn = ftp_connection_create (ftp->addr,
			        G_VFS_JOB (job)->cancellable,
			        &error);
  if (conn == NULL)
    goto fail;

  if (ftp->user &&
      g_vfs_keyring_lookup_password (ftp->user,
				     soup_address_get_name (ftp->addr),
				     NULL,
				     "ftp",
				     NULL,
				     NULL,
				     port == 21 ? 0 : port,
				     &username,
				     NULL,
				     &password))
      goto try_login;

  while (TRUE)
    {
      if (prompt == NULL)
	/* translators: %s here is the hostname */
	prompt = g_strdup_printf (_("Enter password for ftp on %s"), host);

      if (!g_mount_source_ask_password (
			mount_source,
		        prompt,
			ftp->user ? ftp->user : "anonymous",
		        NULL,
		        G_ASK_PASSWORD_NEED_USERNAME |
		        G_ASK_PASSWORD_NEED_PASSWORD |
		        G_ASK_PASSWORD_ANONYMOUS_SUPPORTED |
		        (g_vfs_keyring_is_available () ? G_ASK_PASSWORD_SAVING_SUPPORTED : 0),
		        &aborted,
		        &password,
		        &username,
		        NULL,
		        &password_save) ||
	  aborted) 
	{
	  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		       "%s", _("Password dialog cancelled"));
	  goto fail;
	}

try_login:
      g_free (ftp->user);
      ftp->user = username;
      g_free (ftp->password);
      ftp->password = password;
      if (ftp_connection_login (conn, username, password, &error) != 0)
	break;
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
	goto fail;

      g_error_free (error);
      error = NULL;
    }
  
  if (prompt)
    {
      /* a prompt was created, so we have to save the password */
      g_vfs_keyring_save_password (ftp->user,
                                   soup_address_get_name (ftp->addr),
                                   NULL,
                                   "ftp",
				   NULL,
				   NULL,
				   port == 21 ? 0 : port,
                                   ftp->password,
                                   password_save);
      g_free (prompt);
    }

  if (!ftp_connection_use (conn, &error))
    goto fail;

  mount_spec = g_mount_spec_new ("ftp");
  g_mount_spec_set (mount_spec, "host", soup_address_get_name (ftp->addr));
  if (port != 21)
    {
      char *port_str = g_strdup_printf ("%u", port);
      g_mount_spec_set (mount_spec, "port", port_str);
      g_free (port_str);
    }

  if (g_str_equal (ftp->user, "anonymous"))
    {
      display_name = g_strdup_printf (_("ftp on %s"), host);
    }
  else
    {
      g_mount_spec_set (mount_spec, "user", ftp->user);
      /* Translators: the first %s is the username, the second the host name */
      display_name = g_strdup_printf (_("ftp as %s on %s"), ftp->user, host);
    }
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);

  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  g_vfs_backend_set_icon_name (backend, "folder-remote");

  ftp->queue = g_queue_new ();
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_free (host);
  return;

fail:
  if (conn)
    ftp_connection_free (conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
  g_free (host);
}

static gboolean
try_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  const char *host, *port_str;
  guint port;

  host = g_mount_spec_get (mount_spec, "host");
  if (host == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                       G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("No hostname specified"));
      return TRUE;
    }
  port_str = g_mount_spec_get (mount_spec, "port");
  if (port_str == NULL)
    port = 21;
  else
    {
      /* FIXME: error handling? */
      port = strtoul (port_str, NULL, 10);
    }

  ftp->addr = soup_address_new (host, port);
  ftp->user = g_strdup (g_mount_spec_get (mount_spec, "user"));

  return FALSE;
}

static void
do_unmount (GVfsBackend *   backend,
	    GVfsJobUnmount *job)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;

  g_mutex_lock (ftp->mutex);
  while ((conn = g_queue_pop_head (ftp->queue)))
    {
      /* FIXME: properly quit */
      ftp_connection_free (conn);
    }
  g_queue_free (ftp->queue);
  ftp->queue = NULL;
  g_cond_broadcast (ftp->cond);
  g_mutex_unlock (ftp->mutex);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  guint status;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;
  if (!ftp_connection_ensure_data_connection (conn, &error))
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  status = ftp_connection_send (conn,
				RESPONSE_PASS_100 | RESPONSE_FAIL_200,
				&error,
                                "RETR %s", file);
  g_free (file);
  if (status == 0)
    goto error;

  /* don't push the connection back, it's our handle now */
  conn->cancellable = NULL;
  g_vfs_job_open_for_read_set_handle (job, conn);
  g_vfs_job_open_for_read_set_can_seek (job, FALSE);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_close_read (GVfsBackend *     backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn = handle;
  guint response;

  conn->cancellable = G_VFS_JOB (job)->cancellable;
  ftp_connection_close_data_connection (conn);
  response = ftp_connection_receive (conn, 0, &error); 
  if (response == 0)
    {
      g_vfs_backend_ftp_push_connection (ftp, conn);
      if (response != 0)
	ftp_error_set_from_response (&error, response);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_read (GVfsBackend *     backend,
	 GVfsJobRead *     job,
	 GVfsBackendHandle handle,
	 char *            buffer,
	 gsize             bytes_requested)
{
  GError *error = NULL;
  FtpConnection *conn = handle;
  SoupSocketIOStatus status;
  gsize n_bytes;

  status = soup_socket_read (conn->data,
			     buffer,
			     bytes_requested,
			     &n_bytes,
			     G_VFS_JOB (job)->cancellable,
			     &error);
  switch (status)
    {
      case SOUP_SOCKET_EOF:
      case SOUP_SOCKET_OK:
	g_vfs_job_read_set_size (job, n_bytes);
	g_vfs_job_succeeded (G_VFS_JOB (job));
	return;
      case SOUP_SOCKET_ERROR:
	break;
      case SOUP_SOCKET_WOULD_BLOCK:
      default:
	g_assert_not_reached ();
	break;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static gboolean
do_start_write (GVfsBackendFtp *ftp,
		FtpConnection *conn,
		GVfsJobOpenForWrite *job,
		GFileCreateFlags flags,
		const char *format,
		...) G_GNUC_PRINTF (5, 6);
static gboolean
do_start_write (GVfsBackendFtp *ftp,
		FtpConnection *conn,
		GVfsJobOpenForWrite *job,
		GFileCreateFlags flags,
		const char *format,
		...)
{
  va_list varargs;
  GError *error = NULL;
  guint status;

  /* FIXME: can we honour the flags? */
  if (!ftp_connection_ensure_data_connection (conn, &error))
    goto error;

  va_start (varargs, format);
  status = ftp_connection_sendv (conn,
				RESPONSE_PASS_100 | RESPONSE_FAIL_200,
				&error,
				format,
				varargs);
  va_end (varargs);
  if (status == 0)
    goto error;

  /* don't push the connection back, it's our handle now */
  conn->cancellable = NULL;
  g_vfs_job_open_for_write_set_handle (job, conn);
  g_vfs_job_open_for_write_set_can_seek (job, FALSE);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
  return FALSE;
}

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename,
	   GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  GError *error = NULL;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, job, flags, "STOR %s", file);
  g_free (file);
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_append (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename,
	   GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  GError *error = NULL;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, job, flags, "APPE %s", filename);
  g_free (file);
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    const char *etag,
	    gboolean make_backup,
	    GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  GError *error = NULL;
  FtpFile *file;

  if (make_backup)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			G_IO_ERROR_NOT_SUPPORTED,
			_("backups not supported yet"));
      return;
    }

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, job, flags, "STOR %s", filename);
  g_free (file);
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_close_write (GVfsBackend *backend,
	        GVfsJobCloseWrite *job,
	        GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn = handle;
  guint response;

  conn->cancellable = G_VFS_JOB (job)->cancellable;
  ftp_connection_close_data_connection (conn);
  response = ftp_connection_receive (conn, 0, &error); 
  if (response == 0)
    {
      g_vfs_backend_ftp_push_connection (ftp, conn);
      if (response != 0)
	ftp_error_set_from_response (&error, response);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize buffer_size)
{
  GError *error = NULL;
  FtpConnection *conn = handle;
  SoupSocketIOStatus status;
  gsize n_bytes;

  status = soup_socket_write (conn->data,
			      buffer,
			      buffer_size,
			      &n_bytes,
			      G_VFS_JOB (job)->cancellable,
			      &error);
  switch (status)
    {
      case SOUP_SOCKET_EOF:
      case SOUP_SOCKET_OK:
	g_vfs_job_write_set_written_size (job, n_bytes);
	g_vfs_job_succeeded (G_VFS_JOB (job));
	return;
      case SOUP_SOCKET_ERROR:
	break;
      case SOUP_SOCKET_WOULD_BLOCK:
      default:
	g_assert_not_reached ();
	break;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

#if 0
typedef enum {
  FILE_INFO_SIZE         = (1 << 1),
  FILE_INFO_MTIME	 = (1 << 2),
  FILE_INFO_TYPE         = (1 << 3)
} FileInfoFlags;

static FileInfoFlags
file_info_get_flags (FtpConnection *        conn,
		     GFileAttributeMatcher *matcher)
{
  FileInfoFlags flags = 0;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
      (conn->features& FTP_FEATURE_SIZE))
    flags |= FILE_INFO_SIZE;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
      (conn->features & FTP_FEATURE_MDTM))
    flags |= FILE_INFO_MTIME;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_TYPE))
    flags |= FILE_INFO_TYPE;

  return flags;
}

static void
file_info_query (FtpConnection *conn,
		 const char *filename,
		 GFileInfo *     info,
		 FileInfoFlags  flags)
{
  GFileType type;
  guint response;

  DEBUG ("query %s (flags %u)\n", filename, flags);

  if (flags & FILE_INFO_TYPE)
    {
      /* kind of an evil trick here to determine the type.
       * We cwd to the given filename.
       * If it succeeds, it's a directroy, otherwise it's a file.
       */
      response = ftp_connection_send (conn, 0, NULL, "CWD %s", filename);
      if (response == 0)
	type = G_FILE_TYPE_REGULAR;
      else
	type = G_FILE_TYPE_DIRECTORY;
    }
  else
    type = G_FILE_TYPE_REGULAR;

  gvfs_file_info_populate_default (info, filename, type);

  if (flags & FILE_INFO_SIZE)
    {
      response = ftp_connection_send (conn, 0, NULL, "SIZE %s", filename);

      if (response != 0)
	{
	  guint64 size = g_ascii_strtoull (conn->read_buffer + 4, NULL, 10);
	  g_file_info_set_size (info, size);
	}
    }

  if (flags & FILE_INFO_MTIME)
    {
      response = ftp_connection_send (conn, 0, NULL, "MDTM %s", filename);

      if (response != 0)
	{
	  GTimeVal tv;
	  char *date;

	  /* modify read buffer to get a valid iso time */
	  date = conn->read_buffer + 4;
	  memmove (date + 9, date + 8, 10);
	  date[8] = 'T';
	  date[19] = 0;
	  if (g_time_val_from_iso8601 (date, &tv))
	    g_file_info_set_modification_time (info, &tv);
	  else
	    DEBUG ("not a time: %s\n", date);
	}
    }

}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags query_flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  FileInfoFlags flags;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  g_file_info_set_name (info, filename);
  flags = file_info_get_flags (conn, matcher);
  file_info_query (conn, filename, info, flags);

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags query_flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  char *name;
  gsize size, n_bytes, bytes_read;
  SoupSocketIOStatus status;
  gboolean got_boundary;
  GList *walk, *list = NULL;
  guint response;
  FileInfoFlags flags;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;
  if (!ftp_connection_ensure_data_connection (conn, &error))
    goto error;

  response = ftp_connection_send (conn, 
				  RESPONSE_PASS_100 | RESPONSE_FAIL_200,
				  &error,
				  "NLST %s", filename);
  if (response == 0)
    goto error;

  size = 128;
  bytes_read = 0;
  name = g_malloc (size);

  do
    {
      if (bytes_read + 3 >= size)
	{
	  if (size >= 16384)
	    {
	      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG,
		           _("filename too long"));
	      break;
	    }
	  size += 128;
	  name = g_realloc (name, size);
	}
      status = soup_socket_read_until (conn->data,
				       name + bytes_read,
				       size - bytes_read - 1,
				       "\r\n",
				       2,
				       &n_bytes,
				       &got_boundary,
				       conn->cancellable,
				       &error);

      bytes_read += n_bytes;
      switch (status)
	{
	  case SOUP_SOCKET_EOF:
	  case SOUP_SOCKET_OK:
	    if (n_bytes == 0)
	      {
		status = SOUP_SOCKET_EOF;
		break;
	      }
	    if (got_boundary)
	      {
		name[bytes_read - 2] = 0;
		DEBUG ("file: %s\n", name);
		list = g_list_prepend (list, g_strdup (name));
		bytes_read = 0;
	      }
	    break;
	  case SOUP_SOCKET_ERROR:
	    goto error2;
	  case SOUP_SOCKET_WOULD_BLOCK:
	  default:
	    g_assert_not_reached ();
	    break;
	}
    }
  while (status == SOUP_SOCKET_OK);

  if (bytes_read)
    {
      name[bytes_read] = 0;
      DEBUG ("file: %s\n", name);
      list = g_list_prepend (list, name);
    }
  else
    g_free (name);

  response = ftp_connection_receive (conn, 0, &error);
  if (response == 0)
    goto error;
  ftp_connection_close_data_connection (conn);

  flags = file_info_get_flags (conn, matcher);
  for (walk = list; walk; walk = walk->next)
    {
      GFileInfo *info = g_file_info_new ();
      g_file_info_set_attribute_mask (info, matcher);
      g_file_info_set_name (info, walk->data);
      file_info_query (conn, walk->data, info, flags);
      g_vfs_job_enumerate_add_info (job, info);
      g_free (walk->data);
    }
  g_list_free (list);

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error2:
  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0, NULL);
error:
  g_list_foreach (list, (GFunc) g_free, NULL);
  g_list_free (list);
  ftp_connection_close_data_connection (conn);
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}
#endif

static GFileInfo *
process_line (FtpConnection *conn, const char *line, const FtpFile *dirname, struct list_state *state)
{
  struct list_result result = { 0, };
  GTimeVal tv = { 0, 0 };
  GFileInfo *info;
  int type;
  FtpFile *name;
  char *s, *t;

  DEBUG ("--- %s\n", line);
  type = ParseFTPList (line, state, &result);
  if (type != 'd' && type != 'f' && type != 'l')
    return NULL;

  s = g_strndup (result.fe_fname, result.fe_fnlen);
  if (dirname)
    {
      name = ftp_filename_construct (conn, dirname, s);
      g_free (s);
    }
  else
    name = (FtpFile *) s;
  if (name == NULL)
    return NULL;

  info = g_file_info_new ();

  s = ftp_filename_to_gvfs_path (conn, name);
  t = strrchr (s, '/');
  if (t == NULL || t[1] == 0)
    t = s;
  else
    t++;

  g_file_info_set_name (info, t);

  if (type == 'l')
    {
      char *link;

      g_file_info_set_is_symlink (info, TRUE);

      link = g_strndup (result.fe_lname, result.fe_lnlen);
      if (!ftp_connection_cd (conn, dirname, NULL))
	{
	  g_object_unref (info);
	  g_free (link);
	  g_free (s);
	  g_free (name);
	  return NULL;
	}

      if (ftp_connection_cd (conn, (FtpFile *) s, NULL))
	type = 'd';
      else
	type = 'f';

      /* FIXME: can we just copy paths for symlinks? */
      g_file_info_set_symlink_target (info, link);
    }

  g_file_info_set_size (info, strtoul (result.fe_size, NULL, 10));

  gvfs_file_info_populate_default (info, s,
				   type == 'd' ? G_FILE_TYPE_DIRECTORY :
				   G_FILE_TYPE_REGULAR);
  g_free (s);
  g_free (name);

  tv.tv_sec = mktime (&result.fe_time);
  if (tv.tv_sec != -1)
    g_file_info_set_modification_time (info, &tv);

  return info;
}

static GList *
run_list_command (FtpConnection *conn, GError **error, const char *command, ...) G_GNUC_PRINTF (3, 4);
static GList *
run_list_command (FtpConnection *conn, GError **error, const char *command, ...)
{
  gsize size, n_bytes, bytes_read;
  SoupSocketIOStatus status;
  gboolean got_boundary;
  guint response;
  va_list varargs;
  char *name;
  GList *list = NULL;

  if (!ftp_connection_ensure_data_connection (conn, error))
    goto error;

  va_start (varargs, command);
  response = ftp_connection_sendv (conn, 
				   RESPONSE_PASS_100 | RESPONSE_FAIL_200,
				   error,
				   command,
				   varargs);
  va_end (varargs);
  if (response == 0)
    goto error;

  size = 128;
  bytes_read = 0;
  name = g_malloc (size);

  do
    {
      if (bytes_read + 3 >= size)
	{
	  if (size >= 16384)
	    {
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG,
		           _("filename too long"));
	      break;
	    }
	  size += 128;
	  name = g_realloc (name, size);
	}
      status = soup_socket_read_until (conn->data,
				       name + bytes_read,
				       size - bytes_read - 1,
				       "\r\n",
				       2,
				       &n_bytes,
				       &got_boundary,
				       conn->cancellable,
				       error);

      bytes_read += n_bytes;
      switch (status)
	{
	  case SOUP_SOCKET_EOF:
	  case SOUP_SOCKET_OK:
	    if (n_bytes == 0)
	      {
		status = SOUP_SOCKET_EOF;
		break;
	      }
	    if (got_boundary)
	      {
		name[bytes_read - 2] = 0;
		list = g_list_prepend (list, g_strdup (name));
		bytes_read = 0;
	      }
	    break;
	  case SOUP_SOCKET_ERROR:
	    goto error2;
	  case SOUP_SOCKET_WOULD_BLOCK:
	  default:
	    g_assert_not_reached ();
	    break;
	}
    }
  while (status == SOUP_SOCKET_OK);

  if (bytes_read)
    {
      name[bytes_read] = 0;
      list = g_list_prepend (list, name);
    }
  else
    g_free (name);

  ftp_connection_close_data_connection (conn);
  response = ftp_connection_receive (conn, 0, error);
  if (response == 0)
    goto error;

  return g_list_reverse (list);

error2:
  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0, NULL);
error:
  ftp_connection_close_data_connection (conn);
  g_list_foreach (list, (GFunc) g_free, NULL);
  g_list_free (list);
  return NULL;
}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags query_flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  GList *walk, *list;
  guint response;
  FtpFile *file = NULL;
  struct list_state state = { 0, };

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  response = ftp_connection_cd (conn, file, NULL);
  if (response != 0)
    { 
      /* file is a directory */
      char *basename = g_path_get_basename (filename);

      g_file_info_set_name (info, basename);
      gvfs_file_info_populate_default (info, 
				       basename,
				       G_FILE_TYPE_DIRECTORY);
      g_free (basename);
    }
  else
    {
      GFileInfo *real = NULL;

      /* file is not a directory - maybe it doesn't even exist? */
      list = run_list_command (conn, &error, "LIST %s", file);
      if (error)
	goto error;
      for (walk = list; walk; walk = walk->next)
	{
	  GFileInfo *cur = process_line (conn, walk->data, NULL, &state);
	  g_free (walk->data);
	  if (cur == NULL)
	    continue;
	  if (real != NULL)
	    {
	      g_list_foreach (walk->next, (GFunc) g_free, NULL); 
	      g_list_free (list);
	      g_object_unref (cur);
	      goto error;
	    }
	  real = cur;
	}
      g_list_free (list);
      if (real == NULL)
	{
	  error = g_error_new_literal (G_IO_ERROR,
				       G_IO_ERROR_NOT_FOUND,
				       _("File does not exist"));
	  goto error;
	}

      g_file_info_copy_into (real, info);
      g_object_unref (real);
    }

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_free (file);
  return;

error:
  g_free (file);
  ftp_connection_close_data_connection (conn);
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags query_flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  GList *walk, *list;
  guint response;
  FtpFile *file = NULL;
  struct list_state state = { 0, };

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  file = ftp_filename_from_gvfs_path (conn, filename);
  response = ftp_connection_cd (conn, file, &error);
  if (response == 0)
    goto error;

  list = run_list_command (conn, &error, "LIST");
  if (error)
    goto error;

  for (walk = list; walk; walk = walk->next)
    {
      GFileInfo *info = process_line (conn, walk->data, file, &state);
      if (info)
	{
	  g_vfs_job_enumerate_add_info (job, info);
	  g_object_unref (info);
	}
      g_free (walk->data);
    }
  g_list_free (list);

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_free (file);
  return;

error:
  g_free (file);
  ftp_connection_close_data_connection (conn);
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
do_set_display_name (GVfsBackend *backend,
		     GVfsJobSetDisplayName *job,
		     const char *filename,
		     const char *display_name)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  GError *error = NULL;
  FtpConnection *conn;
  char *name;
  FtpFile *original, *dir, *now;
  guint response;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job)->cancellable, &error);
  if (conn == NULL)
    goto error;

  original = ftp_filename_from_gvfs_path (conn, filename);
  name = g_path_get_dirname (filename);
  dir = ftp_filename_from_gvfs_path (conn, name);
  g_free (name);
  now = ftp_filename_construct (conn, dir, display_name);
  g_free (dir);
  if (now == NULL)
    {
      g_set_error (&error, 
	           G_IO_ERROR,
	           G_IO_ERROR_INVALID_FILENAME,
		   _("Invalid filename"));
      goto error;
    }
  response = ftp_connection_send (conn,
                                  RESPONSE_PASS_300 | RESPONSE_FAIL_200,
				  &error,
				  "RNFR %s", original);
  g_free (original);
  if (response == 0)
    {
      g_free (now);
      goto error;
    }
  response = ftp_connection_send (conn,
				  0,
				  &error,
				  "RNTO %s", now);
  if (response == 0)
    {
      g_free (now);
      goto error;
    }

  name = ftp_filename_to_gvfs_path (conn, now);
  g_free (now);
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_set_display_name_set_new_path (job, name);
  g_free (name);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_free (dir);
  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
g_vfs_backend_ftp_class_init (GVfsBackendFtpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_ftp_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->close_read = do_close_read;
  backend_class->read = do_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append;
  backend_class->replace = do_replace;
  backend_class->close_write = do_close_write;
  backend_class->write = do_write;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
}
