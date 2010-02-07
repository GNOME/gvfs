/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#include <config.h>

#include <stdio.h> /* for sscanf() */
#include <stdlib.h> /* for exit() */

#include <glib/gi18n.h>

#include "gvfsftptask.h"

/*** DOCS ***/

/**
 * GVfsFtpResponseFlags:
 * @G_VFS_FTP_PASS_100: Don't treat 1XX responses, but return them
 * @G_VFS_FTP_PASS_300: Don't treat 3XX responses, but return them
 * @G_VFS_FTP_PASS_400: Don't treat 4XX responses, but return them
 * @G_VFS_FTP_PASS_500: Don't treat 5XX responses, but return them
 * @G_VFS_FTP_PASS_550: Don't treat 550 responses, but return them
 * @G_VFS_FTP_FAIL_200: Fail on a 2XX response
 *
 * These flags can be passed to gvfs_ftp_task_receive() (and in
 * turn gvfs_ftp_task_send()) to influence the behavior of the functions.
 */

/**
 * G_VFS_FTP_G_VFS_FTP_GROUP:
 * @response: a valid ftp response
 *
 * Determines the group the given @response belongs to. The group is the first
 * digit of the reply.
 *
 * Returns: The group the response code belonged to from 1-5
 */

/**
 * G_VFS_FTP_TASK_INIT:
 * @backend: the backend used by this task
 * @job: the job that initiated the task or %NULL if none
 *
 * Initializes a new task structure for the given backend and job.
 */

/**
 * GVfsFtpErrorFunc:
 * @task: task to handle
 * @data: data argument provided to g_vfs_ftp_task_send_and_check()
 *
 * Function prototype for error checking functions used by
 * g_vfs_ftp_task_send_and_check(). When called, these functions are supposed
 * to check a specific error condition and if met, set an error on the passed
 * @task.
 */

/*** CODE ***/

gboolean
g_vfs_ftp_task_login (GVfsFtpTask *task,
                      const char * username,
                      const char * password)
{
  guint status;

  g_return_val_if_fail (task != NULL, FALSE);
  g_return_val_if_fail (username != NULL, FALSE);
  g_return_val_if_fail (password != NULL, FALSE);

  if (g_vfs_ftp_task_is_in_error (task))
    return FALSE;

  status = g_vfs_ftp_task_send (task, G_VFS_FTP_PASS_300,
                                "USER %s", username);
 
  if (G_VFS_FTP_RESPONSE_GROUP (status) == 3)
    {
      /* rationale for choosing the default password:
       * - some ftp servers expect something that looks like an email address
       * - we don't want to send the user's name or address, as that would be
       *   a privacy problem
       * - we want to give ftp server administrators a chance to notify us of
       *   problems with our client.
       * - we don't want to drown in spam.
       */
      if (password == NULL || password[0] == 0)
        password = "gvfsd-ftp-" VERSION "@example.com";
      status = g_vfs_ftp_task_send (task, 0,
        			    "PASS %s", password);
    }

  return status;
}

/**
 * g_vfs_ftp_task_setup_connection:
 * @task: the task
 *
 * Sends all commands necessary to put the connection into a usable state,
 * like setting the transfer mode to binary. Note that passive mode will
 * will be set on a case-by-case basis when opening a data connection.
 **/
void
g_vfs_ftp_task_setup_connection (GVfsFtpTask *task)
{
  g_return_if_fail (task != NULL);

  /* only binary transfers please */
  g_vfs_ftp_task_send (task, 0, "TYPE I");
  if (g_vfs_ftp_task_is_in_error (task))
    return;

#if 0
  /* RFC 2428 suggests to send this to make NAT routers happy */
  /* XXX: Disabled for the following reasons:
   * - most ftp clients don't use it
   * - lots of broken ftp servers can't see the difference between
   *   "EPSV" and "EPSV ALL"
   * - impossible to dynamically fall back to regular PASV in case
   *   EPSV doesn't work for some reason.
   * If this makes your ftp connection fail, please file a bug and we will
   * try to invent a way to make this all work. Until then, we'll just
   * ignore the RFC.
   */
  if (g_vfs_backend_ftp_has_feature (task->backend, g_VFS_FTP_FEATURE_EPSV))
    g_vfs_ftp_task_send (task, 0, "EPSV ALL");
  g_vfs_ftp_task_clear_error (task);
#endif

  /* instruct server that we'll give and assume we get utf8 */
  if (g_vfs_backend_ftp_has_feature (task->backend, G_VFS_FTP_FEATURE_UTF8))
    {
      if (!g_vfs_ftp_task_send (task, 0, "OPTS UTF8 ON"))
        g_vfs_ftp_task_clear_error (task);
    }
}


static void
do_broadcast (GCancellable *cancellable, GCond *cond)
{
  g_cond_broadcast (cond);
}

/**
 * g_vfs_ftp_task_acquire_connection:
 * @task: a task without an associated connection
 *
 * Acquires a new connection for use by this @task. This uses the connection
 * pool of @task's backend, so it reuses previously opened connections and
 * does not reopen new connections unnecessarily. If all connections are busy,
 * it waits %G_VFS_FTP_TIMEOUT_IN_SECONDS seconds for a new connection to
 * become available. Keep in mind that a newly acquired connection might have
 * timed out and therefore closed by the FTP server. You must account for
 * this when sending the first command to the server.
 *
 * Returns: %TRUE if a connection could be acquired, %FALSE if an error
 *          occured
 **/
static gboolean
g_vfs_ftp_task_acquire_connection (GVfsFtpTask *task)
{
  GVfsBackendFtp *ftp;
  GTimeVal now;
  gulong id;

  g_return_val_if_fail (task != NULL, FALSE);
  g_return_val_if_fail (task->conn == NULL, FALSE);

  if (g_vfs_ftp_task_is_in_error (task))
    return FALSE;

  ftp = task->backend;
  g_mutex_lock (ftp->mutex);
  id = g_cancellable_connect (task->cancellable,
        		      G_CALLBACK (do_broadcast),
        		      ftp->cond, NULL);
  while (task->conn == NULL && ftp->queue != NULL)
    {
      if (g_cancellable_is_cancelled (task->cancellable))
        {
          task->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
        		                     _("Operation was cancelled"));
          break;
        }

      task->conn = g_queue_pop_head (ftp->queue);
      if (task->conn != NULL)
        break;

      if (ftp->connections < ftp->max_connections)
        {
          static GThread *last_thread = NULL;
          /* Save current number of connections here, so we can limit maximum
           * connections later.
           * This is necessary for threading reasons (connections can be
           * opened or closed while we are still in the opening process. */
          guint maybe_max_connections = ftp->connections;

          ftp->connections++;
          last_thread = g_thread_self ();
          g_mutex_unlock (ftp->mutex);
          task->conn = g_vfs_ftp_connection_new (ftp->addr, task->cancellable, &task->error);
          if (G_LIKELY (task->conn != NULL))
            {
              g_vfs_ftp_task_receive (task, 0, NULL);
              g_vfs_ftp_task_login (task, ftp->user, ftp->password);
              g_vfs_ftp_task_setup_connection (task);
              if (G_LIKELY (!g_vfs_ftp_task_is_in_error (task)))
                break;
            }

          g_vfs_ftp_connection_free (task->conn);
          task->conn = NULL;
          g_mutex_lock (ftp->mutex);
          ftp->connections--;
          /* If this value is still equal to our thread it means there were no races 
           * trying to open connections and the maybe_max_connections value is 
           * reliable. */
          if (last_thread == g_thread_self () && 
              !g_vfs_ftp_task_error_matches (task, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              g_print ("maybe: %u, max %u (due to %s)\n", maybe_max_connections, ftp->max_connections, task->error->message);
              ftp->max_connections = MIN (ftp->max_connections, maybe_max_connections);
              if (ftp->max_connections == 0)
                {
                  g_debug ("no more connections left, exiting...\n");
                  /* FIXME: shut down properly */
                  exit (0);
                }
            }

          g_vfs_ftp_task_clear_error (task);
          continue;
        }

      g_get_current_time (&now);
      g_time_val_add (&now, G_VFS_FTP_TIMEOUT_IN_SECONDS * 1000 * 1000);
      if (ftp->busy_connections >= ftp->connections ||
          !g_cond_timed_wait (ftp->cond, ftp->mutex, &now))
        {
          task->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BUSY,
        		                     _("The FTP server is busy. Try again later"));
          break;
        }
    }
  g_cancellable_disconnect (task->cancellable, id);
  g_mutex_unlock (ftp->mutex);

  return task->conn != NULL;
}

/**
 * g_vfs_ftp_task_release_connection:
 * @task: a task
 *
 * Releases the connection in use by @task to the backend's connection pool,
 * or frees it if it is in an error state. You must use this function to free
 * a @task's connection, never use g_vfs_ftp_connection_free() directly. If
 * the task does not have a current connection, this function just returns.
 **/
static void
g_vfs_ftp_task_release_connection (GVfsFtpTask *task)
{
  g_return_if_fail (task != NULL);

  /* we allow task->conn == NULL to ease error cases */
  if (task->conn == NULL)
    return;

  g_mutex_lock (task->backend->mutex);
  if (task->backend->queue && g_vfs_ftp_connection_is_usable (task->conn))
    {
      g_queue_push_tail (task->backend->queue, task->conn);
      g_cond_signal (task->backend->cond);
    }
  else
    {
      task->backend->connections--;
      g_vfs_ftp_connection_free (task->conn);
    }
  g_mutex_unlock (task->backend->mutex);
  task->conn = NULL;
}

/**
 * g_vfs_ftp_task_done:
 * @task: the task to finalize
 *
 * Finalizes the given task and clears all memory in use. It also marks the
 * associated job as success or failure depending on the error state of the
 * task.
 **/
void
g_vfs_ftp_task_done (GVfsFtpTask *task)
{
  g_return_if_fail (task != NULL);

  g_vfs_ftp_task_release_connection (task);

  if (task->job)
    {
      if (g_vfs_ftp_task_is_in_error (task))
        g_vfs_job_failed_from_error (task->job, task->error);
      else
        g_vfs_job_succeeded (task->job);
    }

  g_vfs_ftp_task_clear_error (task);
}

/**
 * g_vfs_ftp_task_set_error_from_response:
 * @task: the task
 * @response: the response code
 *
 * Sets the @task into an error state. The exact error is determined from the
 * @response code.
 **/
void
g_vfs_ftp_task_set_error_from_response (GVfsFtpTask *task, guint response)
{
  const char *msg;
  int code;

  g_return_if_fail (task != NULL);
  g_return_if_fail (task->error == NULL);

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
        code = G_IO_ERROR_FAILED;
        msg = _("Operation failed");
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
      case 522: /* EPRT: unsupported network protocol */
        code = G_IO_ERROR_NOT_SUPPORTED;
        msg = _("Unsupported network protocol");
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

  g_set_error_literal (&task->error, G_IO_ERROR, code, msg);
}

/**
 * g_vfs_ftp_task_give_connection:
 * @task: the task
 * @conn: the connection that the @task should use
 *
 * Forces a given @task to do I/O using the given connection. The @task must
 * not have a connection associated with itself. The @task will take
 * ownership of @conn.
 **/
void
g_vfs_ftp_task_give_connection (GVfsFtpTask *      task,
                                GVfsFtpConnection *conn)
{
  g_return_if_fail (task != NULL);
  g_return_if_fail (task->conn == NULL);

  task->conn = conn;
  /* this connection is not busy anymore */
  g_mutex_lock (task->backend->mutex);
  g_assert (task->backend->busy_connections > 0);
  task->backend->busy_connections--;
  g_mutex_unlock (task->backend->mutex);
}

/**
 * g_vfs_ftp_task_take_connection:
 * @task: the task
 *
 * Acquires the connection in use by the @task, so it can later be used with
 * g_vfs_ftp_task_give_connection(). This or any other task will not use the
 * connection anymore. The @task must have a connection in use.
 *
 * Returns: The connection that @task was using. You acquire ownership of
 *          the connection.
 **/
GVfsFtpConnection *
g_vfs_ftp_task_take_connection (GVfsFtpTask *task)
{
  GVfsFtpConnection *conn;
  GVfsBackendFtp *ftp;

  g_return_val_if_fail (task != NULL, NULL);
  g_return_val_if_fail (task->conn != NULL, NULL);

  conn = task->conn;
  task->conn = NULL;

  ftp = task->backend;
  /* mark this connection as busy */
  g_mutex_lock (ftp->mutex);
  ftp->busy_connections++;
  /* if all connections are busy, signal all waiting threads, 
   * so they stop waiting and return BUSY earlier */
  if (ftp->busy_connections >= ftp->connections)
    g_cond_broadcast (ftp->cond);
  g_mutex_unlock (ftp->mutex);

  return conn;
}

/**
 * g_vfs_ftp_task_send:
 * @task: the sending task
 * @flags: response flags to use when sending
 * @format: format string to construct command from
 *          (without trailing \r\n)
 * @...: arguments to format string
 *
 * Shortcut to calling g_vfs_ftp_task_send_and_check() with the reply, funcs
 * and data arguments set to %NULL. See that function for details.
 *
 * Returns: 0 on error or the received FTP code otherwise.
 **/
guint
g_vfs_ftp_task_send (GVfsFtpTask *        task,
                     GVfsFtpResponseFlags flags,
                     const char *         format,
                     ...)
{
  va_list varargs;
  guint response;

  g_return_val_if_fail (task != NULL, 0);
  g_return_val_if_fail (format != NULL, 0);

  va_start (varargs, format);
  response = g_vfs_ftp_task_sendv (task,
        			   flags,
                                   NULL,
        			   format,
        			   varargs);
  va_end (varargs);
  return response;
}

/**
 * g_vfs_ftp_task_send_and_check:
 * @task: the sending task
 * @flags: response flags to use when sending
 * @funcs: %NULL or %NULL-terminated array of functions used to determine the
 *         exact failure case upon a "550 Operation Failed" reply. This is
 *         often necessary
 * @data: data to pass to @funcs.
 * @reply: %NULL or pointer to take a char array containing the full reply of
 *         the ftp server upon successful reply. Use g_strfreev() to free
 *         after use.
 * @format: format string to construct command from
 *          (without trailing \r\n)
 * @...: arguments to format string
 *
 * Takes an ftp command in printf-style @format, potentially acquires a
 * connection automatically, sends the command and waits for an answer from
 * the ftp server. Without any @flags, FTP response codes other than 2xx cause
 * an error. If @reply is not %NULL, the full reply will be put into a
 * %NULL-terminated string array that must be freed with g_strfreev() after
 * use.
 * If @funcs is set, the 550 response code will cause all of these functions to
 * be called in order passing them the @task and @data arguments given to this
 * function until one of them sets an error on @task. This error will then be
 * returned from this function. If none of those functions sets an error, the
 * generic error for the 550 response will be used.
 * If an error has been set on @task previously, this function will do nothing.
 *
 * Returns: 0 on error or the received FTP code otherwise.
 **/
guint
g_vfs_ftp_task_send_and_check (GVfsFtpTask *           task,
                               GVfsFtpResponseFlags    flags,
                               const GVfsFtpErrorFunc *funcs,
                               gpointer                data,
                               char ***                reply,
                               const char *            format,
                               ...)
{
  va_list varargs;
  guint response;

  g_return_val_if_fail (task != NULL, 0);
  g_return_val_if_fail (format != NULL, 0);
  g_return_val_if_fail (funcs == NULL || funcs[0] != NULL, 0);

  if (funcs)
    {
      g_return_val_if_fail ((flags & G_VFS_FTP_PASS_550) == 0, 0);
      flags |= G_VFS_FTP_PASS_550;
    }

  va_start (varargs, format);
  response = g_vfs_ftp_task_sendv (task,
        			   flags,
                                   reply,
        			   format,
        			   varargs);
  va_end (varargs);

  if (response == 550 && funcs)
    {
      /* close a potentially open data connection, the error handlers
       * might try to open new ones and that would cause assertions */
      g_vfs_ftp_task_close_data_connection (task);

      while (*funcs && !g_vfs_ftp_task_is_in_error (task))
        {
          (*funcs) (task, data);
          funcs++;
        }
      if (!g_vfs_ftp_task_is_in_error (task))
          g_vfs_ftp_task_set_error_from_response (task, response);
      response = 0;
    }

  return response;
}

/**
 * g_vfs_ftp_task_sendv:
 * @task: the sending task
 * @flags: response flags to use when receiving the reply
 * @reply: %NULL or pointer to char array that takes the full reply from the
 *         server
 * @format: format string to construct command from
 *          (without trailing \r\n)
 * @varargs: arguments to format string
 *
 * This is the varargs version of g_vfs_ftp_task_send(). See that function
 * for details.
 *
 * Returns: the received FTP code or 0 on error.
 **/
guint
g_vfs_ftp_task_sendv (GVfsFtpTask *          task,
                      GVfsFtpResponseFlags   flags,
                      char ***               reply,
                      const char *           format,
                      va_list                varargs)
{
  GString *command;
  gboolean retry_on_timeout = FALSE;
  guint response;

  if (g_vfs_ftp_task_is_in_error (task))
    return 0;

  command = g_string_new ("");
  g_string_append_vprintf (command, format, varargs);
  g_string_append (command, "\r\n");

retry:
  if (task->conn == NULL)
    {
      if (!g_vfs_ftp_task_acquire_connection (task))
        {
          g_string_free (command, TRUE);
          return 0;
        }
      retry_on_timeout = TRUE;
    }

  g_vfs_ftp_connection_send (task->conn,
                             command->str,
                             command->len,
                             task->cancellable,
                             &task->error);

  response = g_vfs_ftp_task_receive (task, flags, reply);
 
  /* NB: requires adaption if we allow passing 4xx responses */
  if (retry_on_timeout &&
      g_vfs_ftp_task_is_in_error (task) &&
      !g_vfs_ftp_connection_is_usable (task->conn))
    {
      g_vfs_ftp_task_clear_error (task);
      g_vfs_ftp_task_release_connection (task);
      goto retry;
    }

  g_string_free (command, TRUE);
  return response;
}

/**
 * g_vfs_ftp_task_receive:
 * @task: the receiving task
 * @flags: response flags to use
 * @reply: %NULL or pointer to char array that takes the full reply from the
 *         server
 *
 * Unless @task is in an error state, this function receives a reply from
 * the @task's connection. The @task must have a connection set, which will
 * happen when either g_vfs_ftp_task_send() or
 * g_vfs_ftp_task_give_connection() have been called on the @task before.
 * Unless @flags are given, all reply codes not in the 200s cause an error.
 * If @task is in an error state when calling this function, nothing will
 * happen and the function will just return.
 *
 * Returns: the received FTP code or 0 on error.
 **/
guint
g_vfs_ftp_task_receive (GVfsFtpTask *        task,
                        GVfsFtpResponseFlags flags,
                        char ***             reply)
{
  guint response;

  g_return_val_if_fail (task != NULL, 0);
  if (g_vfs_ftp_task_is_in_error (task))
    return 0;
  g_return_val_if_fail (task->conn != NULL, 0);

  response = g_vfs_ftp_connection_receive (task->conn,
                                           reply,
                                           task->cancellable,
                                           &task->error);

  switch (G_VFS_FTP_RESPONSE_GROUP (response))
    {
      case 0:
        return 0;
      case 1:
        if (flags & G_VFS_FTP_PASS_100)
          break;
        g_vfs_ftp_task_set_error_from_response (task, response);
        break;
      case 2:
        if (flags & G_VFS_FTP_FAIL_200)
          g_vfs_ftp_task_set_error_from_response (task, response);
        break;
      case 3:
        if (flags & G_VFS_FTP_PASS_300)
          break;
        g_vfs_ftp_task_set_error_from_response (task, response);
        break;
      case 4:
        g_vfs_ftp_task_set_error_from_response (task, response);
        break;
      case 5:
        if ((flags & G_VFS_FTP_PASS_500) ||
            (response == 550 && (flags & G_VFS_FTP_PASS_550)))
          break;
        g_vfs_ftp_task_set_error_from_response (task, response);
        break;
      default:
        g_assert_not_reached ();
        break;
    }

  if (g_vfs_ftp_task_is_in_error (task))
    {
      if (response != 0 && reply)
        {
          g_strfreev (*reply);
          *reply = NULL;
        }
      response = 0;
    }

  return response;
}

/**
 * g_vfs_ftp_task_close_data_connection:
 * @task: a task potentially having an open data connection
 *
 * Closes any data connection @task might have opened.
 */
void
g_vfs_ftp_task_close_data_connection (GVfsFtpTask *task)
{
  g_return_if_fail (task != NULL);

  if (task->conn == NULL)
    return;

  g_vfs_ftp_connection_close_data_connection (task->conn);
}

static GSocketAddress *
g_vfs_ftp_task_create_remote_address (GVfsFtpTask *task, guint port)
{
  GSocketAddress *old, *new;

  old = g_vfs_ftp_connection_get_address (task->conn, &task->error);
  if (old == NULL)
    return NULL;
  g_assert (G_IS_INET_SOCKET_ADDRESS (old));
  new = g_inet_socket_address_new (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (old)), port);

  return new;
}

static GVfsFtpMethod
g_vfs_ftp_task_setup_data_connection_epsv (GVfsFtpTask *task, GVfsFtpMethod method)
{
  const char *s;
  char **reply;
  guint port;
  GSocketAddress *addr;
  guint status;
  gboolean success;

  g_assert (task->error == NULL);

  status = g_vfs_ftp_task_send_and_check (task, G_VFS_FTP_PASS_500, NULL, NULL, &reply, "EPSV");
  if (G_VFS_FTP_RESPONSE_GROUP (status) != 2)
    return G_VFS_FTP_METHOD_ANY;

  /* FIXME: parse multiple lines? */
  s = strrchr (reply[0], '(');
  if (!s)
    goto fail;

  s += 4;
  port = strtoul (s, NULL, 10);
  if (port == 0)
    goto fail;

  g_strfreev (reply);
  addr = g_vfs_ftp_task_create_remote_address (task, port);
  if (addr == NULL)
    return G_VFS_FTP_METHOD_ANY;

  success = g_vfs_ftp_connection_open_data_connection (task->conn,
                                                       addr,
                                                       task->cancellable,
                                                       &task->error);
  g_object_unref (addr);
  return success ? G_VFS_FTP_METHOD_EPSV : G_VFS_FTP_METHOD_ANY;

fail:
  g_strfreev (reply);
  return G_VFS_FTP_METHOD_ANY;
}

static GVfsFtpMethod
g_vfs_ftp_task_setup_data_connection_pasv (GVfsFtpTask *task, GVfsFtpMethod method)
{
  guint ip1, ip2, ip3, ip4, port1, port2;
  char **reply;
  const char *s;
  GSocketAddress *addr;
  guint status;
  gboolean success;

  status = g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "PASV");
  if (status == 0)
    return G_VFS_FTP_METHOD_ANY;

  /* parse response and try to find the address to connect to.
   * This code does the same as curl.
   */
  for (s = reply[0]; *s; s++)
    {
      if (sscanf (s, "%u,%u,%u,%u,%u,%u",
        	 &ip1, &ip2, &ip3, &ip4,
        	 &port1, &port2) == 6)
       break;
    }
  if (*s == 0)
    {
      g_strfreev (reply);
      g_set_error_literal (&task->error, G_IO_ERROR, G_IO_ERROR_FAILED,
        		   _("Invalid reply"));
      return G_VFS_FTP_METHOD_ANY;
    }
  g_strfreev (reply);

  if (method == G_VFS_FTP_METHOD_PASV || method == G_VFS_FTP_METHOD_ANY)
    {
      guint8 ip[4];
      GInetAddress *inet_addr;

      ip[0] = ip1;
      ip[1] = ip2;
      ip[2] = ip3;
      ip[3] = ip4;
      inet_addr = g_inet_address_new_from_bytes (ip, G_SOCKET_FAMILY_IPV4);
      addr = g_inet_socket_address_new (inet_addr, port1 << 8 | port2);
      g_object_unref (inet_addr);

      success = g_vfs_ftp_connection_open_data_connection (task->conn,
                                                           addr,
                                                           task->cancellable,
                                                           &task->error);
      g_object_unref (addr);
      if (success)
        return G_VFS_FTP_METHOD_PASV;
      if (g_vfs_ftp_task_is_in_error (task) && method != G_VFS_FTP_METHOD_ANY)
        return G_VFS_FTP_METHOD_ANY;

      g_vfs_ftp_task_clear_error (task);
    }

  if (method == G_VFS_FTP_METHOD_PASV_ADDR || method == G_VFS_FTP_METHOD_ANY)
    {
      /* Workaround code:
       * Various ftp servers aren't setup correctly when behind a NAT. They report
       * their own IP address (like 10.0.0.4) and not the address in front of the
       * NAT. But this is likely the same address that we connected to with our
       * command connetion. So if the address given by PASV fails, we fall back
       * to the address of the command stream.
       */
      addr = g_vfs_ftp_task_create_remote_address (task, port1 << 8 | port2);
      if (addr == NULL)
        return G_VFS_FTP_METHOD_ANY;
      success = g_vfs_ftp_connection_open_data_connection (task->conn,
                                                           addr,
                                                           task->cancellable,
                                                           &task->error);
      g_object_unref (addr);
      if (success)
        return G_VFS_FTP_METHOD_PASV_ADDR;
    }

  return G_VFS_FTP_METHOD_ANY;
}

static GVfsFtpMethod
g_vfs_ftp_task_setup_data_connection_eprt (GVfsFtpTask *task, GVfsFtpMethod unused)
{
  GSocketAddress *addr;
  guint status, port, family;
  char *ip_string;

  /* workaround for the task not having a connection yet */
  if (task->conn == NULL &&
      g_vfs_ftp_task_send (task, 0, "NOOP") == 0)
    return G_VFS_FTP_METHOD_ANY;

  addr = g_vfs_ftp_connection_listen_data_connection (task->conn, &task->error);
  if (addr == NULL)
    return G_VFS_FTP_METHOD_ANY;
  switch (g_socket_address_get_family (addr))
    {
      case G_SOCKET_FAMILY_IPV4:
        family = 1;
        break;
      case G_SOCKET_FAMILY_IPV6:
        family = 2;
        break;
      default:
        g_object_unref (addr);
        return G_VFS_FTP_METHOD_ANY;
    }

  ip_string = g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (addr)));
  /* if this ever happens (and it must not for IP4 and IP6 addresses), 
   * we need to add support for using a different separator */
  g_assert (strchr (ip_string, '|') == NULL);
  port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (addr));

  /* we could handle the 522 response here, (unsupported network family),
   * but I don't think that will buy us anything */
  status = g_vfs_ftp_task_send (task, 0, "EPRT |%u|%s|%u|", family, ip_string, port);
  g_free (ip_string);
  g_object_unref (addr);
  if (status == 0)
    return G_VFS_FTP_METHOD_ANY;
  
  return G_VFS_FTP_METHOD_EPRT;
}

static GVfsFtpMethod
g_vfs_ftp_task_setup_data_connection_port (GVfsFtpTask *task, GVfsFtpMethod unused)
{
  GSocketAddress *addr;
  guint status, i, port;
  char *ip_string;

  /* workaround for the task not having a connection yet */
  if (task->conn == NULL &&
      g_vfs_ftp_task_send (task, 0, "NOOP") == 0)
    return G_VFS_FTP_METHOD_ANY;

  addr = g_vfs_ftp_connection_listen_data_connection (task->conn, &task->error);
  if (addr == NULL)
    return G_VFS_FTP_METHOD_ANY;
  /* the PORT command only supports IPv4 */
  if (g_socket_address_get_family (addr) != G_SOCKET_FAMILY_IPV4)
    {
      g_object_unref (addr);
      return G_VFS_FTP_METHOD_ANY;
    }

  ip_string = g_inet_address_to_string (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (addr)));
  for (i = 0; ip_string[i]; i++)
    {
      if (ip_string[i] == '.')
        ip_string[i] = ',';
    }
  port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (addr));

  status = g_vfs_ftp_task_send (task, 0, "PORT %s,%u,%u", ip_string, port >> 8, port & 0xFF);
  g_free (ip_string);
  g_object_unref (addr);
  if (status == 0)
    return G_VFS_FTP_METHOD_ANY;
  
  return G_VFS_FTP_METHOD_PORT;
}

typedef GVfsFtpMethod (* GVfsFtpOpenDataConnectionFunc) (GVfsFtpTask *task, GVfsFtpMethod method);
typedef struct _GVfsFtpOpenDataConnectionMethod GVfsFtpOpenDataConnectionMethod;
struct _GVfsFtpOpenDataConnectionMethod {
    GVfsFtpFeature                required_feature;
    GSocketFamily                 required_family;
    GVfsFtpOpenDataConnectionFunc func;
};

static gboolean
g_vfs_ftp_task_open_data_connection_method_is_supported (const GVfsFtpOpenDataConnectionMethod *method,
                                                         GVfsFtpTask *                          task,
                                                         GSocketFamily                          family)
{
  if (method->required_feature &&
      !g_vfs_backend_ftp_has_feature (task->backend, method->required_feature))
    return FALSE;

  if (method->required_family != G_SOCKET_FAMILY_INVALID &&
      method->required_family != family)
    return FALSE;

  return TRUE;
}

static GSocketFamily
g_vfs_ftp_task_get_socket_family (GVfsFtpTask *task)
{
  GSocketAddress *addr;
  GSocketFamily family;

  /* workaround for the task not having a connection yet */
  if (task->conn == NULL &&
      g_vfs_ftp_task_send (task, 0, "NOOP") == 0)
    {
      g_vfs_ftp_task_clear_error (task);
      return G_SOCKET_FAMILY_INVALID;
    }

  addr = g_vfs_ftp_connection_get_address (task->conn, NULL);
  if (addr == NULL)
    return G_SOCKET_FAMILY_INVALID;

  family = g_socket_address_get_family (addr);
  g_object_unref (addr);
  return family;
}

static GVfsFtpMethod
g_vfs_ftp_task_setup_data_connection_any (GVfsFtpTask *task, GVfsFtpMethod unused)
{
  static const GVfsFtpOpenDataConnectionMethod funcs_ordered[] = {
    { 0,                      G_SOCKET_FAMILY_IPV4,    g_vfs_ftp_task_setup_data_connection_pasv },
    { G_VFS_FTP_FEATURE_EPSV, G_SOCKET_FAMILY_INVALID, g_vfs_ftp_task_setup_data_connection_epsv },
    { 0,                      G_SOCKET_FAMILY_IPV4,    g_vfs_ftp_task_setup_data_connection_port },
    { G_VFS_FTP_FEATURE_EPRT, G_SOCKET_FAMILY_INVALID, g_vfs_ftp_task_setup_data_connection_eprt }
  };
  GVfsFtpMethod method;
  GSocketFamily family;
  guint i;

  family = g_vfs_ftp_task_get_socket_family (task);

  /* first try all advertised features */
  for (i = 0; i < G_N_ELEMENTS (funcs_ordered); i++)
    {
      if (!g_vfs_ftp_task_open_data_connection_method_is_supported (&funcs_ordered[i], task, family))
        continue;
      method = funcs_ordered[i].func (task, G_VFS_FTP_METHOD_ANY);
      if (method != G_VFS_FTP_METHOD_ANY)
        return method;
      
      g_vfs_ftp_task_clear_error (task);
    }

  /* then try if the non-advertised features work */
  for (i = 0; i < G_N_ELEMENTS (funcs_ordered); i++)
    {
      if (g_vfs_ftp_task_open_data_connection_method_is_supported (&funcs_ordered[i], task, family))
        continue;
      method = funcs_ordered[i].func (task, G_VFS_FTP_METHOD_ANY);
      if (method != G_VFS_FTP_METHOD_ANY)
        return method;
      
      g_vfs_ftp_task_clear_error (task);
    }

  /* finally, just give up */
  return G_VFS_FTP_METHOD_ANY;
}

/**
 * g_vfs_ftp_task_setup_data_connection:
 * @task: a task not having an open data connection
 *
 * Sets up a data connection to the ftp server with using the best method for 
 * this task. If the operation fails, @task will be set into an error state.
 * You must call g_vfs_ftp_task_open_data_connection() to finish setup and 
 * ensure the data connection actually gets opened. Usually, this requires 
 * sending an FTP command down the stream.
 **/
void
g_vfs_ftp_task_setup_data_connection (GVfsFtpTask *task)
{
  static const GVfsFtpOpenDataConnectionFunc connect_funcs[] = {
    [G_VFS_FTP_METHOD_ANY]       = g_vfs_ftp_task_setup_data_connection_any,
    [G_VFS_FTP_METHOD_EPSV]      = g_vfs_ftp_task_setup_data_connection_epsv,
    [G_VFS_FTP_METHOD_PASV]      = g_vfs_ftp_task_setup_data_connection_pasv,
    [G_VFS_FTP_METHOD_PASV_ADDR] = g_vfs_ftp_task_setup_data_connection_pasv,
    [G_VFS_FTP_METHOD_EPRT]      = g_vfs_ftp_task_setup_data_connection_eprt,
    [G_VFS_FTP_METHOD_PORT]      = g_vfs_ftp_task_setup_data_connection_port
  };
  GVfsFtpMethod method, result;

  g_return_if_fail (task != NULL);

  task->method = G_VFS_FTP_METHOD_ANY;

  method = g_atomic_int_get (&task->backend->method);
  g_assert (method < G_N_ELEMENTS (connect_funcs) && connect_funcs[method]);

  if (g_vfs_ftp_task_is_in_error (task))
    return;

  result = connect_funcs[method] (task, method);

  /* be sure to try all possibilities if one failed */
  if (result == G_VFS_FTP_METHOD_ANY &&
      method != G_VFS_FTP_METHOD_ANY &&
      !g_vfs_ftp_task_is_in_error (task))
    result = g_vfs_ftp_task_setup_data_connection_any (task, G_VFS_FTP_METHOD_ANY);

  g_assert (result < G_N_ELEMENTS (connect_funcs) && connect_funcs[result]);
  if (result != method)
    {
      static const char *methods[] = {
        [G_VFS_FTP_METHOD_ANY] = "any",
        [G_VFS_FTP_METHOD_EPSV] = "EPSV",
        [G_VFS_FTP_METHOD_PASV] = "PASV",
        [G_VFS_FTP_METHOD_PASV_ADDR] = "PASV with workaround",
        [G_VFS_FTP_METHOD_EPRT] = "EPRT",
        [G_VFS_FTP_METHOD_PORT] = "PORT"
      };
      g_atomic_int_set (&task->backend->method, result);
      g_debug ("# set default data connection method from %s to %s\n",
               methods[method], methods[result]);
    }
  task->method = result;
}

/**
 * g_vfs_ftp_task_open_data_connection:
 * @task: a task
 *
 * Tries to open a data connection to the ftp server. If the operation fails,
 * @task will be set into an error state.
 **/
void
g_vfs_ftp_task_open_data_connection (GVfsFtpTask *task)
{
  g_return_if_fail (task != NULL);

  if (g_vfs_ftp_task_is_in_error (task))
    return;

  if (task->method == G_VFS_FTP_METHOD_EPRT ||
      task->method == G_VFS_FTP_METHOD_PORT)
    g_vfs_ftp_connection_accept_data_connection (task->conn, 
                                                 task->cancellable,
                                                 &task->error);
}

