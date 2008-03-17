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

#include <errno.h> /* for strerror (EAGAIN) */
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

#define PRINT_DEBUG

#ifdef PRINT_DEBUG
#define DEBUG g_print
#else
#define DEBUG(...)
#endif

/* timeout for network connect/send/receive (use 0 for none) */
#define TIMEOUT_IN_SECONDS 30

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

/* unsinged char is on purpose, so we get warnings when we misuse them */
typedef unsigned char FtpFile;
typedef struct _FtpConnection FtpConnection;

typedef struct FtpDirReader FtpDirReader;
struct FtpDirReader {
  void		(* init_data)	(FtpConnection *conn,
				 const FtpFile *dir);
  GFileInfo *	(* get_root)	(FtpConnection *conn);
  gpointer	(* iter_new)	(FtpConnection *conn);
  GFileInfo *	(* iter_process)(gpointer       iter,
				 FtpConnection *conn,
				 const FtpFile *dirname,
				 const FtpFile *must_match_file,
				 const char    *line,
				 char	      **symlink);
  void		(* iter_free)	(gpointer	iter);
};

typedef enum {
  FTP_FEATURE_MDTM = (1 << 0),
  FTP_FEATURE_SIZE = (1 << 1),
  FTP_FEATURE_TVFS = (1 << 2),
  FTP_FEATURE_EPSV = (1 << 3)
} FtpFeatures;
#define FTP_FEATURES_DEFAULT (FTP_FEATURE_EPSV)

typedef enum {
  FTP_SYSTEM_UNKNOWN = 0,
  FTP_SYSTEM_UNIX,
  FTP_SYSTEM_WINDOWS
} FtpSystem;

struct _GVfsBackendFtp
{
  GVfsBackend		backend;

  SoupAddress *		addr;
  char *		user;
  gboolean              has_initial_user;
  char *		password;	/* password or NULL for anonymous */

  /* vfuncs */
  const FtpDirReader *	dir_ops;

  /* connection collection */
  GQueue *		queue;
  GMutex *		mutex;
  GCond *		cond;
  guint			connections;
  guint			max_connections;

  /* caching results from dir queries */
  GStaticRWLock		directory_cache_lock;
  GHashTable *		directory_cache;
};

G_DEFINE_TYPE (GVfsBackendFtp, g_vfs_backend_ftp, G_VFS_TYPE_BACKEND)

#define STATUS_GROUP(status) ((status) / 100)

/*** FTP CONNECTION ***/

struct _FtpConnection
{
  /* per-job data */
  GError *		error;
  GVfsJob *		job;

  FtpFeatures		features;
  FtpSystem		system;

  SoupSocket *		commands;
  gchar *	      	read_buffer;
  gsize			read_buffer_size;
  gsize			read_bytes;

  SoupSocket *		data;
};

static void
ftp_connection_free (FtpConnection *conn)
{
  g_assert (conn->job == NULL);

  if (conn->commands)
    g_object_unref (conn->commands);
  if (conn->data)
    g_object_unref (conn->data);

  g_slice_free (FtpConnection, conn);
}

#define ftp_connection_in_error(conn) ((conn)->error != NULL)

static gboolean
ftp_connection_pop_job (FtpConnection *conn)
{
  gboolean result;

  g_return_val_if_fail (conn->job != NULL, FALSE);

  if (ftp_connection_in_error (conn))
    {
      g_vfs_job_failed_from_error (conn->job, conn->error);
      g_clear_error (&conn->error);
      result = FALSE;
    }
  else
    {
      g_vfs_job_succeeded (conn->job);
      result = TRUE;
    }

  conn->job = NULL;
  return result;
}

static void
ftp_connection_push_job (FtpConnection *conn, GVfsJob *job)
{
  g_return_if_fail (conn->job == NULL);

  /* FIXME: ref the job? */
  conn->job = job;
}

/**
 * ftp_error_set_from_response:
 * @error: pointer to an error to be set or %NULL
 * @response: an FTP response code to use as the error message
 *
 * Sets an error based on an FTP response code.
 **/
static void
ftp_connection_set_error_from_response (FtpConnection *conn, guint response)
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
	/* FIXME: This is a lot of different errors. So we have to pretend to 
	 * be smart here. */
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
	msg = _("Invalid reply");
	break;
    }

  DEBUG ("error: %s\n", msg);
  g_set_error (&conn->error, G_IO_ERROR, code, msg);
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
			ResponseFlags  flags)
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

  g_assert (conn->job != NULL);

  if (ftp_connection_in_error (conn))
    return 0;

  conn->read_bytes = 0;
  while (reply_state != DONE)
    {
      if (conn->read_buffer_size - conn->read_bytes < 128)
	{
	  gsize new_size = conn->read_buffer_size + 1024;
	  /* FIXME: upper limit for size? */
	  gchar *new = g_try_realloc (conn->read_buffer, new_size);
	  if (new)
	    {
	      conn->read_buffer = new;
	      conn->read_buffer_size = new_size;
	    }
	  else
	    {
	      g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid reply"));
	      return 0;
	    }
	}
      last_line = conn->read_buffer + conn->read_bytes;
      status = soup_socket_read_until (conn->commands,
				       last_line,
				       /* -1 byte for nul-termination */
				       conn->read_buffer_size - conn->read_bytes - 1,
				       "\r\n",
				       2,
				       &n_bytes,
				       &got_boundary,
				       conn->job->cancellable,
				       &conn->error);

      conn->read_bytes += n_bytes;
      conn->read_buffer[conn->read_bytes] = 0;
      DEBUG ("<-- %s", last_line);

      switch (status)
	{
	  case SOUP_SOCKET_OK:
	  case SOUP_SOCKET_EOF:
	    if (got_boundary)
	      break;
	    if (n_bytes > 0)
	      continue;
	    g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
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

      if (reply_state == FIRST_LINE)
	{
	  if (n_bytes < 4 ||
	      last_line[0] <= '0' || last_line[0] > '5' ||
	      last_line[1] < '0' || last_line[1] > '9' ||
	      last_line[2] < '0' || last_line[2] > '9')
	    {
	      g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
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
	      g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
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

  switch (STATUS_GROUP (response))
    {
      case 0:
	return 0;
      case 1:
	if (flags & RESPONSE_PASS_100)
	  break;
	ftp_connection_set_error_from_response (conn, response);
	return 0;
      case 2:
	if (flags & RESPONSE_FAIL_200)
	  {
	    ftp_connection_set_error_from_response (conn, response);
	    return 0;
	  }
	break;
      case 3:
	if (flags & RESPONSE_PASS_300)
	  break;
	ftp_connection_set_error_from_response (conn, response);
	return 0;
      case 4:
	if (flags & RESPONSE_PASS_400)
	  break;
	ftp_connection_set_error_from_response (conn, response);
	return 0;
	break;
      case 5:
	if (flags & RESPONSE_PASS_500)
	  break;
	ftp_connection_set_error_from_response (conn, response);
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
		      const char *   format,
		      va_list	     varargs)
{
  GString *command;
  SoupSocketIOStatus status;
  gsize n_bytes;
  guint response;

  g_assert (conn->job != NULL);

  if (ftp_connection_in_error (conn))
    return 0;

  command = g_string_new ("");
  g_string_append_vprintf (command, format, varargs);
#ifdef PRINT_DEBUG
  if (g_str_has_prefix (command->str, "PASS"))
    DEBUG ("--> PASS ***\n");
  else
    DEBUG ("--> %s\n", command->str);
#endif
  g_string_append (command, "\r\n");
  status = soup_socket_write (conn->commands,
			      command->str,
			      command->len,
			      &n_bytes,
			      conn->job->cancellable,
			      &conn->error);
  switch (status)
    {
      case SOUP_SOCKET_OK:
      case SOUP_SOCKET_EOF:
	if (n_bytes == command->len)
	  break;
	g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
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

  response = ftp_connection_receive (conn, flags);
  return response;
}

static guint
ftp_connection_send (FtpConnection *conn,
		     ResponseFlags  flags,
		     const char *   format,
		     ...) G_GNUC_PRINTF (3, 4);
static guint
ftp_connection_send (FtpConnection *conn,
		     ResponseFlags  flags,
		     const char *   format,
		     ...)
{
  va_list varargs;
  guint response;

  va_start (varargs, format);
  response = ftp_connection_sendv (conn,
				   flags,
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
    { "TVFS", FTP_FEATURE_TVFS },
    { "EPSV", FTP_FEATURE_EPSV }
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

/* NB: you must free the connection if it's in error returning from here */
static FtpConnection *
ftp_connection_create (SoupAddress * addr, 
		       GVfsJob *     job)
{
  FtpConnection *conn;
  guint status;

  conn = g_slice_new0 (FtpConnection);
  ftp_connection_push_job (conn, job);

  conn->commands = soup_socket_new ("non-blocking", FALSE,
                                    "remote-address", addr,
				    "timeout", TIMEOUT_IN_SECONDS,
				    NULL);
  status = soup_socket_connect_sync (conn->commands, job->cancellable);
  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* FIXME: better error messages depending on status please */
      g_set_error (&conn->error,
		   G_IO_ERROR,
		   G_IO_ERROR_HOST_NOT_FOUND,
		   _("Could not connect to host"));
    }

  ftp_connection_receive (conn, 0);
  return conn;
}

static guint
ftp_connection_login (FtpConnection *conn,
		      const char *   username,
		      const char *   password)
{
  guint status;

  if (ftp_connection_in_error (conn))
    return 0;

  status = ftp_connection_send (conn, RESPONSE_PASS_300,
                                "USER %s", username);
  
  if (STATUS_GROUP (status) == 3)
    {
      /* rationale for choosing the default password:
       * - some ftp servers expect something that looks like an email address
       * - we don't want to send the user's name or address, as that would be
       *   a privacy problem
       * - we want to give ftp server administrators a chance to notify us of 
       *   problems with our client.
       * - we don't want to drown in spam.
       */
      if (password == NULL)
	password = "gvfsd-ftp-" VERSION "@example.com";
      status = ftp_connection_send (conn, 0,
				    "PASS %s", password);
    }

  return status;
}

static void
ftp_connection_parse_system (FtpConnection *conn)
{
  static const struct {
    const char *id;
    FtpSystem	system;
  } known_systems[] = {
    /* NB: the first entry that matches is taken, so order matters */
    { "UNIX ", FTP_SYSTEM_UNIX },
    { "WINDOWS_NT ", FTP_SYSTEM_WINDOWS }
  };
  guint i;
  char *system_name = conn->read_buffer + 4;

  for (i = 0; i < G_N_ELEMENTS (known_systems); i++) 
    {
      if (g_ascii_strncasecmp (system_name, 
	                       known_systems[i].id, 
			       strlen (known_systems[i].id)) == 0)
	{
	  conn->system = known_systems[i].system;
	  DEBUG ("system is %u\n", conn->system);
	  break;
	}
    }
}

static gboolean
ftp_connection_use (FtpConnection *conn)
{
  /* only binary transfers please */
  ftp_connection_send (conn, 0, "TYPE I");
  if (ftp_connection_in_error (conn))
    return FALSE;

  /* check supported features */
  if (ftp_connection_send (conn, 0, "FEAT") != 0)
    ftp_connection_parse_features (conn);
  else
    conn->features = FTP_FEATURES_DEFAULT;

  /* RFC 2428 suggests to send this to make NAT routers happy */
  if (conn->features & FTP_FEATURE_EPSV)
    ftp_connection_send (conn, 0, "EPSV ALL");
  g_clear_error (&conn->error);

  if (ftp_connection_send (conn, 0, "SYST"))
    ftp_connection_parse_system (conn);
  g_clear_error (&conn->error);

  return TRUE;
}

static gboolean
ftp_connection_ensure_data_connection (FtpConnection *conn)
{
  guint ip1, ip2, ip3, ip4, port1, port2;
  SoupAddress *addr;
  const char *s;
  char *ip;
  guint status;

  if (conn->features & FTP_FEATURE_EPSV)
    {
      status = ftp_connection_send (conn, RESPONSE_PASS_500, "EPSV");
      if (STATUS_GROUP (status) == 2)
	{
	  s = strrchr (conn->read_buffer, '(');
	  if (s)
	    {
	      guint port;
	      s += 4;
	      port = strtoul (s, NULL, 10);
	      if (port != 0)
		{
		  addr = soup_address_new (
		      soup_address_get_name (soup_socket_get_remote_address (conn->commands)),
		      port);
		  goto have_address;
		}
	    }
	}
    }
  /* only binary transfers please */
  status = ftp_connection_send (conn, 0, "PASV");
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
      g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Invalid reply"));
      return FALSE;
    }
  ip = g_strdup_printf ("%u.%u.%u.%u", ip1, ip2, ip3, ip4);
  addr = soup_address_new (ip, port1 << 8 | port2);
  g_free (ip);

have_address:
  conn->data = soup_socket_new ("non-blocking", FALSE,
				"remote-address", addr,
				"timeout", TIMEOUT_IN_SECONDS,
				NULL);
  g_object_unref (addr);
  status = soup_socket_connect_sync (conn->data, conn->job->cancellable);
  if (!SOUP_STATUS_IS_SUCCESSFUL (status))
    {
      /* FIXME: better error messages depending on status please */
      g_set_error (&conn->error,
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

  return (FtpFile *) g_build_path ("/", (char *) dirname, basename, NULL);
}

#define ftp_filename_equal g_str_equal

/*** COMMON FUNCTIONS WITH SPECIAL HANDLING ***/

static gboolean
ftp_connection_cd (FtpConnection *conn, const FtpFile *file)
{
  guint response = ftp_connection_send (conn,
					RESPONSE_PASS_500,
					"CWD %s", file);
  if (response == 550)
    {
      g_set_error (&conn->error, 
	           G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
		   _("The file is not a directory"));
      response = 0;
    }
  else if (STATUS_GROUP (response) == 5)
    {
      ftp_connection_set_error_from_response (conn, response);
      response = 0;
    }

  return response != 0;
}

static gboolean
ftp_connection_try_cd (FtpConnection *conn, const FtpFile *file)
{
  if (ftp_connection_in_error (conn))
    return FALSE;

  if (!ftp_connection_cd (conn, file))
    {
      g_clear_error (&conn->error);
      return FALSE;
    }
  
  return TRUE;
}

/*** default directory reading ***/

static void
dir_default_init_data (FtpConnection *conn, const FtpFile *dir)
{
  ftp_connection_cd (conn, dir);
  ftp_connection_ensure_data_connection (conn);

  ftp_connection_send (conn,
		       RESPONSE_PASS_100 | RESPONSE_FAIL_200,
		       "LIST");
}

static GFileInfo *
dir_default_get_root (FtpConnection *conn)
{
  GFileInfo *info;
  GIcon *icon;
  char *display_name;
  
  info = g_file_info_new ();
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);

  g_file_info_set_name (info, "/");
  display_name = g_strdup_printf (_("/ on %s"),
      soup_address_get_name (soup_socket_get_remote_address (conn->commands)));
  g_file_info_set_display_name (info, display_name);
  g_free (display_name);
  g_file_info_set_edit_name (info, "/");

  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "inode/directory");

  icon = g_themed_icon_new ("folder-remote");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  return info;
}

static gpointer
dir_default_iter_new (FtpConnection *conn)
{
  return g_slice_new (struct list_state);
}

static GFileInfo *
dir_default_iter_process (gpointer        iter,
			  FtpConnection  *conn,
			  const FtpFile  *dirname,
			  const FtpFile  *must_match_file,
			  const char     *line,
			  char		**symlink)
{
  struct list_state *state = iter;
  struct list_result result = { 0, };
  GTimeVal tv = { 0, 0 };
  GFileInfo *info;
  int type;
  FtpFile *name;
  char *s, *t;

  type = ParseFTPList (line, state, &result);
  if (type != 'd' && type != 'f' && type != 'l')
    return NULL;

  /* don't list . and .. directories
   * Let's hope they're not important files on some ftp servers
   */
  if (type == 'd')
    {
      if (result.fe_fnlen == 1 && 
	  result.fe_fname[0] == '.')
	return NULL;
      if (result.fe_fnlen == 2 && 
	  result.fe_fname[0] == '.' &&
	  result.fe_fname[1] == '.')
	return NULL;
    }

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

  if (must_match_file && !ftp_filename_equal (name, must_match_file))
    {
      g_free (name);
      return NULL;
    }

  info = g_file_info_new ();

  s = ftp_filename_to_gvfs_path (conn, name);

  t = g_path_get_basename (s);
  g_file_info_set_name (info, t);
  g_free (t);

  if (type == 'l')
    {
      char *link;

      link = g_strndup (result.fe_lname, result.fe_lnlen);

      /* FIXME: this whole stuff is not FtpFile save */
      g_file_info_set_symlink_target (info, link);
      g_file_info_set_is_symlink (info, TRUE);

      if (symlink)
	{
	  char *str = g_path_get_dirname (s);
	  char *symlink_file = g_build_path ("/", str, link, NULL);

	  g_free (str);
	  while ((str = strstr (symlink_file, "/../")))
	    {
	      char *end = str + 4;
	      char *start;
	      start = str - 1;
	      while (*start != '/')
		start--;
	      memcpy (start + 1, end, strlen (end) + 1);
	    }
	  str = symlink_file + strlen (symlink_file) - 1;
	  while (*str == '/' && str > symlink_file)
	    *str-- = 0;
	  *symlink = symlink_file;
	}
      g_free (link);
    }
  else if (symlink)
    *symlink = NULL;

  g_file_info_set_size (info, strtoul (result.fe_size, NULL, 10));

  gvfs_file_info_populate_default (info, s,
				   type == 'f' ? G_FILE_TYPE_REGULAR :
				   type == 'l' ? G_FILE_TYPE_SYMBOLIC_LINK :
				   G_FILE_TYPE_DIRECTORY);

  if (conn->system == FTP_SYSTEM_UNIX)
    g_file_info_set_is_hidden (info, result.fe_fnlen > 0 &&
	                             result.fe_fname[0] == '.');

  g_free (s);
  g_free (name);

  tv.tv_sec = mktime (&result.fe_time);
  if (tv.tv_sec != -1)
    g_file_info_set_modification_time (info, &tv);

  return info;
}

static void
dir_default_iter_free (gpointer iter)
{
  g_slice_free (struct list_state, iter);
}

static const FtpDirReader dir_default = {
  dir_default_init_data,
  dir_default_get_root,
  dir_default_iter_new,
  dir_default_iter_process,
  dir_default_iter_free
};

/*** BACKEND ***/

static void
g_vfs_backend_ftp_push_connection (GVfsBackendFtp *ftp, FtpConnection *conn)
{
  /* we allow conn == NULL to ease error cases */
  if (conn == NULL)
    return;

  if (conn->job)
    ftp_connection_pop_job (conn);

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
				  GVfsJob *	  job)
{
  FtpConnection *conn = NULL;
  GTimeVal now;
  guint id;

  g_mutex_lock (ftp->mutex);
  id = g_signal_connect (job->cancellable, 
			       "cancelled", 
			       G_CALLBACK (do_broadcast),
			       ftp->cond);
  while (conn == NULL && ftp->queue != NULL)
    {
      if (g_cancellable_is_cancelled (job->cancellable))
	break;
      conn = g_queue_pop_head (ftp->queue);

      if (conn != NULL) {
	/* Figure out if this connection had a timeout sent. If so, skip it. */
	g_mutex_unlock (ftp->mutex);
	ftp_connection_push_job (conn, job);
	if (ftp_connection_send (conn, 0, "NOOP"))
	  break;
	    
	g_clear_error (&conn->error);
	conn->job = NULL;
	ftp_connection_free (conn);
	conn = NULL;
	g_mutex_lock (ftp->mutex);
	ftp->connections--;
	continue;
      }

      if (ftp->connections < ftp->max_connections)
	{
	  ftp->connections++;
	  g_mutex_unlock (ftp->mutex);
	  conn = ftp_connection_create (ftp->addr, job);
	  ftp_connection_login (conn, ftp->user, ftp->password);
	  ftp_connection_use (conn);
	  if (!ftp_connection_in_error (conn))
	    break;

	  ftp_connection_pop_job (conn);
	  ftp_connection_free (conn);
	  conn = NULL;
	  g_mutex_lock (ftp->mutex);
	  ftp->connections--;
	  /* FIXME: This assignment is racy due to the mutex unlock above */
	  ftp->max_connections = ftp->connections;
	  if (ftp->max_connections == 0)
	    {
	      DEBUG ("no more connections left, exiting...");
	      /* FIXME: shut down properly */
	      exit (0);
	    }

	  continue;
	}

      g_get_current_time (&now);
      g_time_val_add (&now, TIMEOUT_IN_SECONDS * 1000 * 1000);
      if (!g_cond_timed_wait (ftp->cond, ftp->mutex, &now))
	{
	  g_vfs_job_failed (G_VFS_JOB (job),
			   G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
			   /* defeat string freeze! */
			   /* _("Resource temporarily unavailable")); */
			   "%s", g_strerror (EAGAIN));
	  break;
	}
    }
  g_signal_handler_disconnect (job->cancellable, id);

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

  g_hash_table_destroy (ftp->directory_cache);
  g_static_rw_lock_free (&ftp->directory_cache_lock);

  g_free (ftp->user);
  g_free (ftp->password);

  if (G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_ftp_parent_class)->finalize) (object);
}

static void
list_free (gpointer list)
{
  g_list_foreach (list, (GFunc) g_free, NULL);
  g_list_free (list);
}

static void
g_vfs_backend_ftp_init (GVfsBackendFtp *ftp)
{
  ftp->mutex = g_mutex_new ();
  ftp->cond = g_cond_new ();

  ftp->directory_cache = g_hash_table_new_full (g_str_hash,
					        g_str_equal,
						g_free,
						list_free);
  g_static_rw_lock_init (&ftp->directory_cache_lock);

  ftp->dir_ops = &dir_default;
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
  gboolean aborted, anonymous, break_on_fail;
  GPasswordSave password_save = G_PASSWORD_SAVE_NEVER;
  guint port;

  conn = ftp_connection_create (ftp->addr,
			        G_VFS_JOB (job));
  /* fail fast here. No need to ask for a password if we know the hostname
   * doesn't exist or the given host/port doesn't have an ftp server running.
   */
  if (ftp_connection_in_error (conn))
    {
      ftp_connection_pop_job (conn);
      ftp_connection_free (conn);
      return;
    }

  port = soup_address_get_port (ftp->addr);
  /* FIXME: need to translate this? */
  if (port == 21)
    host = g_strdup (soup_address_get_name (ftp->addr));
  else
    host = g_strdup_printf ("%s:%u", 
	                    soup_address_get_name (ftp->addr),
	                    port);

  username = NULL;
  password = NULL;
  break_on_fail = FALSE;
  
  if (ftp->user != NULL && strcmp (ftp->user, "anonymous") == 0)
    {
      anonymous = TRUE;
      break_on_fail = TRUE;
      goto try_login;
    }
  
  if (g_vfs_keyring_lookup_password (ftp->user,
				     soup_address_get_name (ftp->addr),
				     NULL,
				     "ftp",
				     NULL,
				     NULL,
				     port == 21 ? 0 : port,
				     &username,
				     NULL,
				     &password))
    {
      anonymous = FALSE;
      goto try_login;
    }

  while (TRUE)
    {
      GAskPasswordFlags flags;
      if (prompt == NULL)
	/* translators: %s here is the hostname */
	prompt = g_strdup_printf (_("Enter password for ftp on %s"), host);

      flags = G_ASK_PASSWORD_NEED_PASSWORD;
        
      if (!ftp->has_initial_user)
        flags |= G_ASK_PASSWORD_NEED_USERNAME | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED;
      
      if (g_vfs_keyring_is_available ())
        flags |= G_ASK_PASSWORD_SAVING_SUPPORTED;
      
      if (!g_mount_source_ask_password (
			mount_source,
		        prompt,
			ftp->user,
		        NULL,
                        flags,
		        &aborted,
		        &password,
		        &username,
		        NULL,
			&anonymous,
		        &password_save) ||
	  aborted) 
	{
	  g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		       "%s", _("Password dialog cancelled"));
	  break;
	}

      /* NEED_USERNAME wasn't set */
      if (ftp->has_initial_user)
        {
          g_free (username);
          username = g_strdup (ftp->user);
        }
      
try_login:
      g_free (ftp->user);
      g_free (ftp->password);
      if (anonymous)
	{
	  if (ftp_connection_login (conn, "anonymous", "") != 0)
	    {
	      ftp->user = g_strdup ("anonymous");
	      ftp->password = g_strdup ("");
	      break;
	    }
	  ftp->user = NULL;
	  ftp->password = NULL;
	}
      else
	{
	  ftp->user = username ? g_strdup (username) : g_strdup ("");
	  ftp->password = g_strdup (password);
	  if (ftp_connection_login (conn, username, password) != 0)
	    break;
	}
      g_free (username);
      g_free (password);
      
      if (break_on_fail ||
          !g_error_matches (conn->error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
	break;

      g_clear_error (&conn->error);
    }
  
  ftp_connection_use (conn);

  if (ftp_connection_in_error (conn))
    {
      ftp_connection_pop_job (conn);
      ftp_connection_free (conn);
    }
  else
    {
      if (prompt && !anonymous)
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

      mount_spec = g_mount_spec_new ("ftp");
      g_mount_spec_set (mount_spec, "host", soup_address_get_name (ftp->addr));
      if (port != 21)
	{
	  char *port_str = g_strdup_printf ("%u", port);
	  g_mount_spec_set (mount_spec, "port", port_str);
	  g_free (port_str);
	}

      if (ftp->has_initial_user)
        g_mount_spec_set (mount_spec, "user", ftp->user);
          
      if (g_str_equal (ftp->user, "anonymous"))
        display_name = g_strdup_printf (_("ftp on %s"), host);
      else
	{
	  /* Translators: the first %s is the username, the second the host name */
	  display_name = g_strdup_printf (_("ftp as %s on %s"), ftp->user, host);
	}
      g_vfs_backend_set_mount_spec (backend, mount_spec);
      g_mount_spec_unref (mount_spec);

      g_vfs_backend_set_display_name (backend, display_name);
      g_free (display_name);
      g_vfs_backend_set_icon_name (backend, "folder-remote");

      ftp->connections = 1;
      ftp->max_connections = G_MAXUINT;
      ftp->queue = g_queue_new ();
      g_vfs_backend_ftp_push_connection (ftp, conn);
    }

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
  ftp->has_initial_user = ftp->user != NULL;

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
  FtpConnection *conn;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (!conn)
    return;

  ftp_connection_ensure_data_connection (conn);

  file = ftp_filename_from_gvfs_path (conn, filename);
  ftp_connection_send (conn,
		       RESPONSE_PASS_100 | RESPONSE_FAIL_200,
		       "RETR %s", file);
  g_free (file);

  if (ftp_connection_in_error (conn))
    g_vfs_backend_ftp_push_connection (ftp, conn);
  else
    {
      /* don't push the connection back, it's our handle now */
      g_vfs_job_open_for_read_set_handle (job, conn);
      g_vfs_job_open_for_read_set_can_seek (job, FALSE);
      ftp_connection_pop_job (conn);
    }
}

static void
do_close_read (GVfsBackend *     backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn = handle;

  ftp_connection_push_job (conn, G_VFS_JOB (job));
  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0); 
  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_read (GVfsBackend *     backend,
	 GVfsJobRead *     job,
	 GVfsBackendHandle handle,
	 char *            buffer,
	 gsize             bytes_requested)
{
  FtpConnection *conn = handle;
  gsize n_bytes;

  ftp_connection_push_job (conn, G_VFS_JOB (job));

  soup_socket_read (conn->data,
		    buffer,
		    bytes_requested,
		    &n_bytes,
		    conn->job->cancellable,
		    &conn->error);
  /* no need to check return value, code will just do the right thing
   * depenging on wether conn->error is set */

  g_vfs_job_read_set_size (job, n_bytes);
  ftp_connection_pop_job (conn);
}

static void
do_start_write (GVfsBackendFtp *ftp,
		FtpConnection *conn,
		GFileCreateFlags flags,
		const char *format,
		...) G_GNUC_PRINTF (4, 5);
static void
do_start_write (GVfsBackendFtp *ftp,
		FtpConnection *conn,
		GFileCreateFlags flags,
		const char *format,
		...)
{
  va_list varargs;
  guint status;

  /* FIXME: can we honour the flags? */

  ftp_connection_ensure_data_connection (conn);

  va_start (varargs, format);
  status = ftp_connection_sendv (conn,
				RESPONSE_PASS_100 | RESPONSE_FAIL_200,
				format,
				varargs);
  va_end (varargs);

  if (ftp_connection_in_error (conn))
    g_vfs_backend_ftp_push_connection (ftp, conn);
  else
    {
      /* don't push the connection back, it's our handle now */
      g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (conn->job), conn);
      g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (conn->job), FALSE);
      ftp_connection_pop_job (conn);
    }
}

static void
gvfs_backend_ftp_purge_cache_directory (GVfsBackendFtp *ftp,
					const FtpFile * dir)
{
  g_static_rw_lock_writer_lock (&ftp->directory_cache_lock);
  g_hash_table_remove (ftp->directory_cache, dir);
  g_static_rw_lock_writer_unlock (&ftp->directory_cache_lock);
}

static void
gvfs_backend_ftp_purge_cache_of_file (GVfsBackendFtp *ftp,
				      FtpConnection * conn,
				      const FtpFile * file)
{
  char *dirname, *filename;
  FtpFile *dir;

  filename = ftp_filename_to_gvfs_path (conn, file);
  dirname = g_path_get_dirname (filename);
  dir = ftp_filename_from_gvfs_path (conn, dirname);

  gvfs_backend_ftp_purge_cache_directory (ftp, dir);

  g_free (dir);
  g_free (filename);
  g_free (dirname);
}

/* forward declaration */
static GFileInfo *
create_file_info (GVfsBackendFtp *ftp, FtpConnection *conn, const char *filename, char **symlink);

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename,
	   GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  GFileInfo *info;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  info = create_file_info (ftp, conn, filename, NULL);
  if (info)
    {
      g_object_unref (info);
      g_set_error (&conn->error,
	           G_IO_ERROR,
		   G_IO_ERROR_EXISTS,
		   _("Target file already exists"));
      goto error;
    }
  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, flags, "STOR %s", file);
  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, file);
  g_free (file);
  return;

error:
  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_append (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename,
	   GFileCreateFlags flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, flags, "APPE %s", filename);
  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, file);
  g_free (file);
  return;
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
  FtpFile *file;

  if (make_backup)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			G_IO_ERROR_CANT_CREATE_BACKUP,
			_("backups not supported yet"));
      return;
    }

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  file = ftp_filename_from_gvfs_path (conn, filename);
  do_start_write (ftp, conn, flags, "STOR %s", file);
  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, file);
  g_free (file);
  return;
}

static void
do_close_write (GVfsBackend *backend,
	        GVfsJobCloseWrite *job,
	        GVfsBackendHandle handle)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn = handle;

  ftp_connection_push_job (conn, G_VFS_JOB (job));

  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0); 

  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle handle,
	  char *buffer,
	  gsize buffer_size)
{
  FtpConnection *conn = handle;
  gsize n_bytes;

  ftp_connection_push_job (conn, G_VFS_JOB (job));

  soup_socket_write (conn->data,
		     buffer,
		     buffer_size,
		     &n_bytes,
		     G_VFS_JOB (job)->cancellable,
		     &conn->error);
  
  g_vfs_job_write_set_written_size (job, n_bytes);
  ftp_connection_pop_job (conn);
}

static GList *
do_enumerate_directory (FtpConnection *conn)
{
  gsize size, n_bytes, bytes_read;
  SoupSocketIOStatus status;
  gboolean got_boundary;
  char *name;
  GList *list = NULL;

  if (ftp_connection_in_error (conn))
    return NULL;

  size = 128;
  bytes_read = 0;
  name = g_malloc (size);

  do
    {
      if (bytes_read + 3 >= size)
	{
	  if (size >= 16384)
	    {
	      g_set_error (&conn->error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG,
		           _("filename too long"));
	      break;
	    }
	  size += 128;
	  name = g_realloc (name, size);
	}
      status = soup_socket_read_until (conn->data,
				       name + bytes_read,
				       size - bytes_read - 1,
				       "\n",
				       1,
				       &n_bytes,
				       &got_boundary,
				       conn->job->cancellable,
				       &conn->error);

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
		name[bytes_read - 1] = 0;
		DEBUG ("--- %s\n", name);
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
      DEBUG ("--- %s\n", name);
      list = g_list_prepend (list, name);
    }
  else
    g_free (name);

  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0);
  if (ftp_connection_in_error (conn))
    goto error;

  return g_list_reverse (list);

error2:
  ftp_connection_close_data_connection (conn);
  ftp_connection_receive (conn, 0);
error:
  ftp_connection_close_data_connection (conn);
  g_list_foreach (list, (GFunc) g_free, NULL);
  g_list_free (list);
  return NULL;
}

/* IMPORTANT: SUCK ALARM!
 * locks ftp->directory_cache_lock but only iff it returns !NULL */
static const GList *
enumerate_directory (GVfsBackendFtp *ftp,
                     FtpConnection * conn,
		     const FtpFile * dir,
		     gboolean	     use_cache)
{
  GList *files;

  g_static_rw_lock_reader_lock (&ftp->directory_cache_lock);
  do {
    if (use_cache)
      files = g_hash_table_lookup (ftp->directory_cache, dir);
    else
      {
	use_cache = TRUE;
	files = NULL;
      }
    if (files == NULL)
      {
	g_static_rw_lock_reader_unlock (&ftp->directory_cache_lock);
	ftp->dir_ops->init_data (conn, dir);
	files = do_enumerate_directory (conn);
	if (files == NULL)
	  {
	    return NULL;
	  }
	g_static_rw_lock_writer_lock (&ftp->directory_cache_lock);
	g_hash_table_insert (ftp->directory_cache, g_strdup ((const char *) dir), files);
	g_static_rw_lock_writer_unlock (&ftp->directory_cache_lock);
	files = NULL;
	g_static_rw_lock_reader_lock (&ftp->directory_cache_lock);
      }
  } while (files == NULL);

  return files;
}

/* NB: This gets a file info for the given object, no matter if it's a dir 
 * or a file */
static GFileInfo *
create_file_info (GVfsBackendFtp *ftp, FtpConnection *conn, const char *filename, char **symlink)
{
  const GList *walk, *files;
  char *dirname;
  FtpFile *dir, *file;
  GFileInfo *info;
  gpointer iter;

  if (symlink)
    *symlink = NULL;

  if (g_str_equal (filename, "/"))
    return ftp->dir_ops->get_root (conn);

  dirname = g_path_get_dirname (filename);
  dir = ftp_filename_from_gvfs_path (conn, dirname);
  g_free (dirname);

  files = enumerate_directory (ftp, conn, dir, TRUE);
  if (files == NULL)
    {
      g_free (dir);
      return NULL;
    }

  file = ftp_filename_from_gvfs_path (conn, filename);
  iter = ftp->dir_ops->iter_new (conn);
  for (walk = files; walk; walk = walk->next)
    {
      info = ftp->dir_ops->iter_process (iter,
					 conn,
					 dir,
					 file,
					 walk->data,
					 symlink);
      if (info)
	break;
    }
  ftp->dir_ops->iter_free (iter);
  g_static_rw_lock_reader_unlock (&ftp->directory_cache_lock);
  g_free (dir);
  g_free (file);
  return info;
}

static GFileInfo *
resolve_symlink (GVfsBackendFtp *ftp, FtpConnection *conn, GFileInfo *original, const char *filename)
{
  GFileInfo *info = NULL;
  char *symlink, *newlink;
  guint i;
  static const char *copy_attributes[] = {
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
    G_FILE_ATTRIBUTE_STANDARD_NAME,
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
    G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME,
    G_FILE_ATTRIBUTE_STANDARD_COPY_NAME,
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET
  };

  if (ftp_connection_in_error (conn))
    return original;

  /* How many symlinks should we follow?
   * <alex> maybe 8?
   */
  symlink = g_strdup (filename);
  for (i = 0; i < 8 && symlink; i++)
    {
      info = create_file_info (ftp,
			       conn,
			       symlink,
			       &newlink);
      if (!newlink)
	break;

      g_free (symlink);
      symlink = newlink;
    }
  g_free (symlink);

  if (ftp_connection_in_error (conn))
    {
      g_assert (info == NULL);
      g_clear_error (&conn->error);
      return original;
    }
  if (info == NULL)
    return original;

  for (i = 0; i < G_N_ELEMENTS (copy_attributes); i++)
    {
      GFileAttributeType type;
      gpointer value;

      if (!g_file_info_get_attribute_data (original,
					   copy_attributes[i],
					   &type,
					   &value,
					   NULL))
	continue;
      
      g_file_info_set_attribute (info,
	                         copy_attributes[i],
				 type,
				 value);
    }
  g_object_unref (original);

  return info;
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
  FtpConnection *conn;
  GFileInfo *real;
  char *symlink;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  if (query_flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
    {
      real = create_file_info (ftp,
			       conn,
			       filename,
			       NULL);
    }
  else
    {
      real = create_file_info (ftp,
			       conn,
			       filename,
			       &symlink);
      if (symlink)
	{
	  real = resolve_symlink (ftp, conn, real, symlink);
	  g_free (symlink);
	}
    }

  if (real)
    {
      g_file_info_copy_into (real, info);
      g_object_unref (real);
    }
  else if (!ftp_connection_in_error (conn))
    g_set_error (&conn->error,
		 G_IO_ERROR,
		 G_IO_ERROR_NOT_FOUND,
		 _("File doesn't exist"));

  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *dirname,
	      GFileAttributeMatcher *matcher,
	      GFileQueryInfoFlags query_flags)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  const GList *walk, *files;
  FtpFile *dir;
  gpointer iter;
  GSList *symlink_targets = NULL;
  GSList *symlink_fileinfos = NULL;
  GSList *twalk, *fwalk;
  GFileInfo *info;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  /* no need to check for IS_DIR, because the enumeration code will return that
   * automatically.
   */

  dir = ftp_filename_from_gvfs_path (conn, dirname);
  files = enumerate_directory (ftp, conn, dir, FALSE);
  if (ftp_connection_pop_job (conn))
    {
      ftp_connection_push_job (conn, G_VFS_JOB (job));
      if (files != NULL)
	{
	  iter = ftp->dir_ops->iter_new (conn);
	  for (walk = files; walk; walk = walk->next)
	    {
	      char *symlink = NULL;
	      info = ftp->dir_ops->iter_process (iter,
						 conn,
						 dir,
						 NULL,
						 walk->data,
						 query_flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS ? NULL : &symlink);
	      if (symlink)
		{
		  /* This is necessary due to our locking. 
		   * And we must not unlock here because it might invalidate the list we iterate */
		  symlink_targets = g_slist_prepend (symlink_targets, symlink);
		  symlink_fileinfos = g_slist_prepend (symlink_fileinfos, info);
		}
	      else if (info)
		{
		  g_vfs_job_enumerate_add_info (job, info);
		  g_object_unref (info);
		}
	    }
	  ftp->dir_ops->iter_free (iter);
	  g_static_rw_lock_reader_unlock (&ftp->directory_cache_lock);
	  for (twalk = symlink_targets, fwalk = symlink_fileinfos; twalk; 
	       twalk = twalk->next, fwalk = fwalk->next)
	    {
	      info = resolve_symlink (ftp, conn, fwalk->data, twalk->data);
	      g_free (twalk->data);
	      g_vfs_job_enumerate_add_info (job, info);
	      g_object_unref (info);
	    }
	  g_slist_free (symlink_targets);
	  g_slist_free (symlink_fileinfos);
	}
      
      g_vfs_job_enumerate_done (job);
      conn->job = NULL;
      g_clear_error (&conn->error);
    }
  else
    g_assert (files == NULL);

  g_vfs_backend_ftp_push_connection (ftp, conn);
  g_free (dir);
}

static void
do_set_display_name (GVfsBackend *backend,
		     GVfsJobSetDisplayName *job,
		     const char *filename,
		     const char *display_name)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  char *name;
  FtpFile *original, *dir, *now;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  original = ftp_filename_from_gvfs_path (conn, filename);
  name = g_path_get_dirname (filename);
  dir = ftp_filename_from_gvfs_path (conn, name);
  g_free (name);
  now = ftp_filename_construct (conn, dir, display_name);
  if (now == NULL)
    {
      g_set_error (&conn->error, 
	           G_IO_ERROR,
	           G_IO_ERROR_INVALID_FILENAME,
		   _("Invalid filename"));
    }
  ftp_connection_send (conn,
		       RESPONSE_PASS_300 | RESPONSE_FAIL_200,
		       "RNFR %s", original);
  g_free (original);
  ftp_connection_send (conn,
		       0,
		       "RNTO %s", now);

  name = ftp_filename_to_gvfs_path (conn, now);
  g_free (now);
  g_vfs_job_set_display_name_set_new_path (job, name);
  g_free (name);
  gvfs_backend_ftp_purge_cache_directory (ftp, dir);
  g_free (dir);
  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_delete (GVfsBackend *backend,
	   GVfsJobDelete *job,
	   const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  FtpFile *file;
  guint response;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  /* We try file deletion first. If that fails, we try directory deletion.
   * The file-first-then-directory order has been decided by coin-toss. */
  file = ftp_filename_from_gvfs_path (conn, filename);
  response = ftp_connection_send (conn,
				  RESPONSE_PASS_500,
				  "DELE %s", file);
  if (STATUS_GROUP (response) == 5)
    {
      response = ftp_connection_send (conn,
				      RESPONSE_PASS_500,
				      "RMD %s", file);
      if (response == 550)
	{
	  const GList *files = enumerate_directory (ftp, conn, file, FALSE);
	  if (files)
	    {
	      g_static_rw_lock_reader_unlock (&ftp->directory_cache_lock);
	      g_set_error (&conn->error, 
			   G_IO_ERROR,
			   G_IO_ERROR_NOT_EMPTY,
			   "%s", g_strerror (ENOTEMPTY));
	    }
	  else
	    ftp_connection_set_error_from_response (conn, response);
	}
      else if (STATUS_GROUP (response) == 5)
	{
	  ftp_connection_set_error_from_response (conn, response);
	}
    }

  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, file);
  g_free (file);
  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_make_directory (GVfsBackend *backend,
		   GVfsJobMakeDirectory *job,
		   const char *filename)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  FtpFile *file;

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  file = ftp_filename_from_gvfs_path (conn, filename);
  ftp_connection_send (conn,
		       0,
		       "MKD %s", file);
  /* FIXME: Compare created file with name from server result to be sure 
   * it's correct and otherwise fail. */
  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, file);
  g_free (file);

  g_vfs_backend_ftp_push_connection (ftp, conn);
}

static void
do_move (GVfsBackend *backend,
	 GVfsJobMove *job,
	 const char *source,
	 const char *destination,
	 GFileCopyFlags flags,
	 GFileProgressCallback progress_callback,
	 gpointer progress_callback_data)
{
  GVfsBackendFtp *ftp = G_VFS_BACKEND_FTP (backend);
  FtpConnection *conn;
  FtpFile *srcfile, *destfile;

  /* FIXME: what about G_FILE_COPY_NOFOLLOW_SYMLINKS and G_FILE_COPY_ALL_METADATA? */

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* FIXME: implement! */
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR,
			G_IO_ERROR_CANT_CREATE_BACKUP,
			_("backups not supported yet"));
      return;
    }

  conn = g_vfs_backend_ftp_pop_connection (ftp, G_VFS_JOB (job));
  if (conn == NULL)
    return;

  srcfile = ftp_filename_from_gvfs_path (conn, source);
  destfile = ftp_filename_from_gvfs_path (conn, destination);
  if (ftp_connection_try_cd (conn, destfile))
    {
      char *basename = g_path_get_basename (source);
      FtpFile *real = ftp_filename_construct (conn, destfile, basename);

      g_free (basename);
      if (real == NULL)
	g_set_error (&conn->error, 
	             G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
		     _("Invalid destination filename"));
      else
	{
	  g_free (destfile);
	  destfile = real;
	}
    }

  if (!(flags & G_FILE_COPY_OVERWRITE))
    {
      char *destfilename = ftp_filename_to_gvfs_path (conn, destfile);
      GFileInfo *info = create_file_info (ftp, conn, destfilename, NULL);

      g_free (destfilename);
      if (info)
	{
	  g_object_unref (info);
	  g_set_error (&conn->error,
		       G_IO_ERROR,
	               G_IO_ERROR_EXISTS,
		       _("Target file already exists"));
	  goto out;
	}
    }

  ftp_connection_send (conn,
		       RESPONSE_PASS_300 | RESPONSE_FAIL_200,
		       "RNFR %s", srcfile);
  ftp_connection_send (conn,
		       0,
		       "RNTO %s", destfile);

  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, srcfile);
  gvfs_backend_ftp_purge_cache_of_file (ftp, conn, destfile);
out:
  g_free (srcfile);
  g_free (destfile);
  g_vfs_backend_ftp_push_connection (ftp, conn);
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
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
}
