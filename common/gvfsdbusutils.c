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

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <gvfsdbusutils.h>
#include <gio/gio.h>

void
_g_dbus_oom (void)
{
  g_error ("DBus failed with out of memory error");
}

/* We use _ for escaping, so its not valid */
#define VALID_INITIAL_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') )
#define VALID_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z'))


static void
append_escaped_name (GString *s,
		     const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  first = TRUE;
  while ((c = *unescaped++) != 0)
    {
      if (first)
	{
	  if (VALID_INITIAL_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}

DBusMessage *
_dbus_message_new_gerror (DBusMessage      *message,
			  GQuark            domain,
			  gint              code,
			  const gchar      *format,
			  ...)
{
  DBusMessage *reply;
  va_list args;
  GError error;
  
  error.domain = domain;
  error.code = code;
  va_start (args, format);
  error.message = g_strdup_vprintf (format, args);
  va_end (args);

  reply = _dbus_message_new_from_gerror (message, &error);

  g_free (error.message);
  
  return reply;
}

DBusMessage *
_dbus_message_new_from_gerror (DBusMessage *message,
				     GError *error)
{
  DBusMessage *reply;
  GString *str;

  str = g_string_new ("org.glib.GError.");
  append_escaped_name (str, g_quark_to_string (error->domain));
  g_string_append_printf (str, ".c%d", error->code);
  reply = dbus_message_new_error (message, str->str, error->message);
  g_string_free (str, TRUE);
  return reply;
}

gboolean
_g_error_from_message (DBusMessage      *message,
		       GError          **error)
{
  DBusError derror;
  
  dbus_error_init (&derror);
  if (dbus_set_error_from_message (&derror, message))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return TRUE;
    }
  return FALSE;
}



static void
append_unescaped_dbus_name (GString *s,
			    const char *escaped,
			    const char *end)
{
  guchar c;

  while (escaped < end)
    {
      c = *escaped++;
      if (c == '_' &&
	  escaped < end)
	{
	  c = g_ascii_xdigit_value (*escaped++) << 4;

	  if (escaped < end)
	    c |= g_ascii_xdigit_value (*escaped++);
	}
      g_string_append_c (s, c);
    }
}

void
_g_dbus_message_iter_append_cstring (DBusMessageIter *iter, const char *str)
{
  DBusMessageIter array;

  if (str == NULL)
    str = "";
  
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE_AS_STRING,
					 &array))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_fixed_array (&array,
					     DBUS_TYPE_BYTE,
					     &str, strlen (str)))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_close_container (iter, &array))
    _g_dbus_oom ();
}

void
_g_dbus_message_iter_append_args_valist (DBusMessageIter *iter,
					 int          first_arg_type,
					 va_list      var_args)
{
  int type;

  g_return_if_fail (iter != NULL);

  type = first_arg_type;

  while (type != DBUS_TYPE_INVALID)
    {
      if (type == G_DBUS_TYPE_CSTRING)
	{
	  const char **value_p;
	  const char *value;

	  value_p = va_arg (var_args, const char**);
	  value = *value_p;

	  _g_dbus_message_iter_append_cstring (iter, value);
	}
      else if (dbus_type_is_basic (type))
        {
          const void *value;
          value = va_arg (var_args, const void*);

          if (!dbus_message_iter_append_basic (iter,
                                               type,
                                               value))
	    _g_dbus_oom ();
        }
      else if (type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          DBusMessageIter array;
          char buf[2];

          element_type = va_arg (var_args, int);
              
          buf[0] = element_type;
          buf[1] = '\0';
          if (!dbus_message_iter_open_container (iter,
                                                 DBUS_TYPE_ARRAY,
                                                 buf,
                                                 &array))
	    _g_dbus_oom ();
          
          if (dbus_type_is_fixed (element_type))
            {
              const void **value;
              int n_elements;

              value = va_arg (var_args, const void**);
              n_elements = va_arg (var_args, int);
              
              if (!dbus_message_iter_append_fixed_array (&array,
                                                         element_type,
                                                         value,
                                                         n_elements))
		_g_dbus_oom ();
            }
          else if (element_type == DBUS_TYPE_STRING ||
                   element_type == DBUS_TYPE_SIGNATURE ||
                   element_type == DBUS_TYPE_OBJECT_PATH)
            {
              const char ***value_p;
              const char **value;
              int n_elements;
              int i;
              
              value_p = va_arg (var_args, const char***);
              n_elements = va_arg (var_args, int);

              value = *value_p;
              
              i = 0;
              while (i < n_elements)
                {
                  if (!dbus_message_iter_append_basic (&array,
                                                       element_type,
                                                       &value[i]))
		    _g_dbus_oom ();
                  ++i;
                }
            }
          else
            {
              g_error ("arrays of %d can't be appended with _g_dbus_message_append_args_valist for now\n",
		       element_type);
            }

          if (!dbus_message_iter_close_container (iter, &array))
	    _g_dbus_oom ();
        }

      type = va_arg (var_args, int);
    }
}


void
_g_dbus_message_append_args_valist (DBusMessage *message,
				    int          first_arg_type,
				    va_list      var_args)
{
  int type;
  DBusMessageIter iter;

  g_return_if_fail (message != NULL);

  type = first_arg_type;

  dbus_message_iter_init_append (message, &iter);

  _g_dbus_message_iter_append_args_valist (&iter,
					   first_arg_type,
					   var_args);
}

dbus_bool_t
_g_dbus_message_iter_get_args_valist (DBusMessageIter *iter,
				      DBusError       *error,
				      int              first_arg_type,
				      va_list          var_args)
{
  int spec_type, msg_type, i, dbus_spec_type;
  dbus_bool_t retval;
  

  retval = FALSE;

  spec_type = first_arg_type;
  i = 0;

  while (spec_type != DBUS_TYPE_INVALID)
    {
      msg_type = dbus_message_iter_get_arg_type (iter);

      if (spec_type == G_DBUS_TYPE_CSTRING)
	dbus_spec_type = DBUS_TYPE_ARRAY;
      else
	dbus_spec_type = spec_type;
      
      if (msg_type != dbus_spec_type)
	{
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Argument %d is specified to be of type \"%c\", but "
                          "is actually of type \"%c\"\n", i,
                          spec_type,
                          msg_type);

          goto out;
	}

      if (spec_type == G_DBUS_TYPE_CSTRING)
	{
          int element_type;
          char **ptr;
	  const char *str;
          int n_elements;
          DBusMessageIter array;

          element_type = dbus_message_iter_get_element_type (iter);
          if (DBUS_TYPE_BYTE != element_type)
            {
              dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                              "Argument %d is specified to be an array of \"char\", but "
                              "is actually an array of \"%d\"\n",
                              i,
                              element_type);
              goto out;
            }

	  ptr = va_arg (var_args, char**);
	  g_assert (ptr != NULL);

	  dbus_message_iter_recurse (iter, &array);
	  dbus_message_iter_get_fixed_array (&array,
					     &str, &n_elements);
	  *ptr = g_strndup (str, n_elements);
	}
      else if (dbus_type_is_basic (spec_type))
        {
          void *ptr;

          ptr = va_arg (var_args, void*);

          g_assert (ptr != NULL);

	  dbus_message_iter_get_basic (iter, ptr);
        }
      else if (spec_type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          int spec_element_type;
          const void **ptr;
          int *n_elements_p;
          DBusMessageIter array;

          spec_element_type = va_arg (var_args, int);
          element_type = dbus_message_iter_get_element_type (iter);

          if (spec_element_type != element_type)
            {
              dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                              "Argument %d is specified to be an array of \"%d\", but "
                              "is actually an array of \"%d\"\n",
                              i,
                              spec_element_type,
                              element_type);

              goto out;
            }

          if (dbus_type_is_fixed (spec_element_type))
            {
              ptr = va_arg (var_args, const void**);
              n_elements_p = va_arg (var_args, int*);

              g_assert (ptr != NULL);
              g_assert (n_elements_p != NULL);

              dbus_message_iter_recurse (iter, &array);

              dbus_message_iter_get_fixed_array (&array,
						 ptr, n_elements_p);
            }
          else if (spec_element_type == DBUS_TYPE_STRING ||
                   spec_element_type == DBUS_TYPE_SIGNATURE ||
                   spec_element_type == DBUS_TYPE_OBJECT_PATH)
            {
              char ***str_array_p;
              int n_elements;
              char **str_array;

              str_array_p = va_arg (var_args, char***);
              n_elements_p = va_arg (var_args, int*);

              g_assert (str_array_p != NULL);
              g_assert (n_elements_p != NULL);

              /* Count elements in the array */
              dbus_message_iter_recurse (iter, &array);

              n_elements = 0;
              while (dbus_message_iter_get_arg_type (&array) != DBUS_TYPE_INVALID)
                {
                  ++n_elements;
                  dbus_message_iter_next (&array);
                }

              str_array = g_new0 (char*, n_elements + 1);
              if (str_array == NULL)
                {
                  _g_dbus_oom ();
                  goto out;
                }

              /* Now go through and dup each string */
              dbus_message_iter_recurse (iter, &array);

              i = 0;
              while (i < n_elements)
                {
                  const char *s;
                  dbus_message_iter_get_basic (&array, &s);
                  
                  str_array[i] = g_strdup (s);
                  if (str_array[i] == NULL)
                    {
                      g_strfreev (str_array);
		      _g_dbus_oom ();
                      goto out;
                    }
                  
                  ++i;
                  
                  if (!dbus_message_iter_next (&array))
                    g_assert (i == n_elements);
                }

              g_assert (dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_INVALID);
              g_assert (i == n_elements);
              g_assert (str_array[i] == NULL);

              *str_array_p = str_array;
              *n_elements_p = n_elements;
            }
        }

      spec_type = va_arg (var_args, int);
      if (!dbus_message_iter_next (iter) && spec_type != DBUS_TYPE_INVALID)
        {
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Message has only %d arguments, but more were expected", i);
          goto out;
        }

      i++;
    }

  retval = TRUE;

 out:
  return retval;
}

dbus_bool_t
_g_dbus_message_iter_get_args (DBusMessageIter *iter,
			       DBusError       *error,
			       int              first_arg_type,
			       ...)
{
  va_list var_args;
  dbus_bool_t res;

  va_start (var_args, first_arg_type);
  res = _g_dbus_message_iter_get_args_valist (iter, error,
					      first_arg_type,
					      var_args);
  va_end (var_args);
  return res;
}

/* Same as the dbus one, except doesn't give OOM and handles
   G_DBUS_TYPE_CSTRING
*/
void
_g_dbus_message_append_args (DBusMessage *message,
			     int          first_arg_type,
			     ...)
{
  va_list var_args;

  g_return_if_fail (message != NULL);

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);
}

void
_g_dbus_message_iter_append_args (DBusMessageIter *iter,
				  int          first_arg_type,
				  ...)
{
  va_list var_args;

  g_return_if_fail (iter != NULL);

  va_start (var_args, first_arg_type);
  _g_dbus_message_iter_append_args_valist (iter,
					   first_arg_type,
					   var_args);
  va_end (var_args);
}

void
_g_error_from_dbus (DBusError *derror, 
		    GError **error)
{
  const char *name, *end;
  GString *str;
  GQuark domain;
  int code;

  if (g_str_has_prefix (derror->name, "org.glib.GError."))
    {
      domain = 0;
      code = 0;

      name = derror->name + strlen ("org.glib.GError.");
      end = strchr (name, '.');
      if (end)
	{
	  str = g_string_new (NULL);
	  append_unescaped_dbus_name (str, name, end);
	  domain = g_quark_from_string (str->str);
	  g_string_free (str, TRUE);

	  end++; /* skip . */
	  if (*end++ == 'c')
	    code = atoi (end);
	}
      
      g_set_error_literal (error, domain, code, derror->message);
    }
  /* TODO: Special case other types, like DBUS_ERROR_NO_MEMORY etc? */
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		 "DBus error %s: %s", derror->name, derror->message);
}

GList *
_g_dbus_bus_list_names_with_prefix (DBusConnection *connection,
				    const char *prefix,
				    DBusError *error)
{
  DBusMessage *message, *reply;
  DBusMessageIter iter, array;
  GList *names;

  g_return_val_if_fail (connection != NULL, NULL);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                          DBUS_PATH_DBUS,
                                          DBUS_INTERFACE_DBUS,
                                          "ListNames");
  if (message == NULL)
    return NULL;
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, error);
  dbus_message_unref (message);
  
  if (reply == NULL)
    return NULL;
  
  names = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) ||
      (dbus_message_iter_get_element_type (&iter) != DBUS_TYPE_STRING))
    goto out;

  for (dbus_message_iter_recurse (&iter, &array);  
       dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRING;
       dbus_message_iter_next (&array))
    {
      char *name;
      dbus_message_iter_get_basic (&array, &name);
      if (g_str_has_prefix (name, prefix))
	names = g_list_prepend (names, g_strdup (name));
    }

  names = g_list_reverse (names);
  
 out:
  dbus_message_unref (reply);
  return names;
}

/*************************************************************************
 *             Helper fd source                                          *
 ************************************************************************/

typedef struct 
{
  GSource source;
  GPollFD pollfd;
  GCancellable *cancellable;
  gulong cancelled_tag;
} FDSource;

static gboolean 
fd_source_prepare (GSource  *source,
		   gint     *timeout)
{
  FDSource *fd_source = (FDSource *)source;
  *timeout = -1;
  
  return g_cancellable_is_cancelled (fd_source->cancellable);
}

static gboolean 
fd_source_check (GSource  *source)
{
  FDSource *fd_source = (FDSource *)source;

  return
    g_cancellable_is_cancelled  (fd_source->cancellable) ||
    fd_source->pollfd.revents != 0;
}

static gboolean
fd_source_dispatch (GSource     *source,
		    GSourceFunc  callback,
		    gpointer     user_data)

{
  GFDSourceFunc func = (GFDSourceFunc)callback;
  FDSource *fd_source = (FDSource *)source;

  g_assert (func != NULL);

  return (*func) (user_data, fd_source->pollfd.revents, fd_source->pollfd.fd);
}

static void 
fd_source_finalize (GSource *source)
{
  FDSource *fd_source = (FDSource *)source;

  if (fd_source->cancelled_tag)
    g_cancellable_disconnect (fd_source->cancellable,
			      fd_source->cancelled_tag);

  if (fd_source->cancellable)
    g_object_unref (fd_source->cancellable);
}

static GSourceFuncs fd_source_funcs = {
  fd_source_prepare,
  fd_source_check,
  fd_source_dispatch,
  fd_source_finalize
};

/* Might be called on another thread */
static void
fd_source_cancelled_cb (GCancellable *cancellable,
			gpointer data)
{
  /* Wake up the mainloop in case we're waiting on async calls with FDSource */
  g_main_context_wakeup (NULL);
}

/* Two __ to avoid conflict with gio version */
GSource *
__g_fd_source_new (int fd,
		   gushort events,
		   GCancellable *cancellable)
{
  GSource *source;
  FDSource *fd_source;

  source = g_source_new (&fd_source_funcs, sizeof (FDSource));
  fd_source = (FDSource *)source;

  if (cancellable)
    fd_source->cancellable = g_object_ref (cancellable);
  
  fd_source->pollfd.fd = fd;
  fd_source->pollfd.events = events;
  g_source_add_poll (source, &fd_source->pollfd);

  if (cancellable)
    fd_source->cancelled_tag =
      g_cancellable_connect (cancellable,
			     (GCallback)fd_source_cancelled_cb,
			     NULL, NULL);
  
  return source;
}


/*************************************************************************
 *                                                                       *
 *      dbus mainloop integration for async ops                          *
 *                                                                       *
 *************************************************************************/

static gint32 main_integration_data_slot = -1;
static GOnce once_init_main_integration = G_ONCE_INIT;

/**
 * A GSource subclass for dispatching DBusConnection messages.
 * We need this on top of the IO handlers, because sometimes
 * there are messages to dispatch queued up but no IO pending.
 * 
 * The source is owned by the connection (and the main context
 * while that is alive)
 */
typedef struct 
{
  GSource source;
  
  DBusConnection *connection;
  GSList *ios;
  GSList *timeouts;
} DBusSource;

typedef struct
{
  DBusSource *dbus_source;
  GSource *source;
  DBusWatch *watch;
} IOHandler;

typedef struct
{
  DBusSource *dbus_source;
  GSource *source;
  DBusTimeout *timeout;
} TimeoutHandler;

static gpointer
main_integration_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&main_integration_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
}

static gboolean
dbus_source_prepare (GSource *source,
		     gint    *timeout)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;
  
  *timeout = -1;

  return (dbus_connection_get_dispatch_status (connection) == DBUS_DISPATCH_DATA_REMAINS);  
}

static gboolean
dbus_source_check (GSource *source)
{
  return FALSE;
}

static gboolean
dbus_source_dispatch (GSource     *source,
		      GSourceFunc  callback,
		      gpointer     user_data)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;

  dbus_connection_ref (connection);

  /* Only dispatch once - we don't want to starve other GSource */
  dbus_connection_dispatch (connection);
  
  dbus_connection_unref (connection);

  return TRUE;
}

static gboolean
io_handler_dispatch (gpointer data,
                     GIOCondition condition,
                     int fd)
{
  IOHandler *handler = data;
  guint dbus_condition = 0;
  DBusConnection *connection;

  connection = handler->dbus_source->connection;
  
  if (connection)
    dbus_connection_ref (connection);
  
  if (condition & G_IO_IN)
    dbus_condition |= DBUS_WATCH_READABLE;
  if (condition & G_IO_OUT)
    dbus_condition |= DBUS_WATCH_WRITABLE;
  if (condition & G_IO_ERR)
    dbus_condition |= DBUS_WATCH_ERROR;
  if (condition & G_IO_HUP)
    dbus_condition |= DBUS_WATCH_HANGUP;

  /* Note that we don't touch the handler after this, because
   * dbus may have disabled the watch and thus killed the
   * handler.
   */
  dbus_watch_handle (handler->watch, dbus_condition);
  handler = NULL;

  if (connection)
    dbus_connection_unref (connection);
  
  return TRUE;
}

static void
io_handler_free (IOHandler *handler)
{
  DBusSource *dbus_source;
  
  dbus_source = handler->dbus_source;
  dbus_source->ios = g_slist_remove (dbus_source->ios, handler);
  
  g_source_destroy (handler->source);
  g_source_unref (handler->source);
  g_free (handler);
}

static void
dbus_source_add_watch (DBusSource *dbus_source,
		       DBusWatch *watch)
{
  guint flags;
  GIOCondition condition;
  IOHandler *handler;
  int fd;

  if (!dbus_watch_get_enabled (watch))
    return;
  
  g_assert (dbus_watch_get_data (watch) == NULL);
  
  flags = dbus_watch_get_flags (watch);

  condition = G_IO_ERR | G_IO_HUP;
  if (flags & DBUS_WATCH_READABLE)
    condition |= G_IO_IN;
  if (flags & DBUS_WATCH_WRITABLE)
    condition |= G_IO_OUT;

  handler = g_new0 (IOHandler, 1);
  handler->dbus_source = dbus_source;
  handler->watch = watch;

#if (DBUS_MAJOR_VERSION == 1 && DBUS_MINOR_VERSION == 1 && DBUS_MICRO_VERSION >= 1) || (DBUS_MAJOR_VERSION == 1 && DBUS_MINOR_VERSION > 1) || (DBUS_MAJOR_VERSION > 1)
  fd = dbus_watch_get_unix_fd (watch);
#else
  fd = dbus_watch_get_fd (watch);
#endif
    
  handler->source = __g_fd_source_new (fd, condition, NULL);
  g_source_set_callback (handler->source,
			 (GSourceFunc) io_handler_dispatch, handler,
                         NULL);
  g_source_attach (handler->source, NULL);
 
  dbus_source->ios = g_slist_prepend (dbus_source->ios, handler);
  dbus_watch_set_data (watch, handler,
		       (DBusFreeFunction)io_handler_free);
}

static void
dbus_source_remove_watch (DBusSource *dbus_source,
			  DBusWatch *watch)
{
  dbus_watch_set_data (watch, NULL, NULL);
}

static void
timeout_handler_free (TimeoutHandler *handler)
{
  DBusSource *dbus_source;

  dbus_source = handler->dbus_source;
  dbus_source->timeouts = g_slist_remove (dbus_source->timeouts, handler);

  g_source_destroy (handler->source);
  g_source_unref (handler->source);
  g_free (handler);
}

static gboolean
timeout_handler_dispatch (gpointer      data)
{
  TimeoutHandler *handler = data;

  dbus_timeout_handle (handler->timeout);
  
  return TRUE;
}

static void
dbus_source_add_timeout (DBusSource *dbus_source,
			 DBusTimeout *timeout)
{
  TimeoutHandler *handler;
  
  if (!dbus_timeout_get_enabled (timeout))
    return;
  
  g_assert (dbus_timeout_get_data (timeout) == NULL);

  handler = g_new0 (TimeoutHandler, 1);
  handler->dbus_source = dbus_source;
  handler->timeout = timeout;

  handler->source = g_timeout_source_new (dbus_timeout_get_interval (timeout));
  g_source_set_callback (handler->source,
			 timeout_handler_dispatch, handler,
                         NULL);
  g_source_attach (handler->source, NULL);

  /* handler->source is owned by the context here */
  dbus_source->timeouts = g_slist_prepend (dbus_source->timeouts, handler);

  dbus_timeout_set_data (timeout, handler,
			 (DBusFreeFunction)timeout_handler_free);
}

static void
dbus_source_remove_timeout (DBusSource *source,
			    DBusTimeout *timeout)
{
  dbus_timeout_set_data (timeout, NULL, NULL);
}

static dbus_bool_t
add_watch (DBusWatch *watch,
	   gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_add_watch (dbus_source, watch);
  
  return TRUE;
}

static void
remove_watch (DBusWatch *watch,
	      gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_remove_watch (dbus_source, watch);
}

static void
watch_toggled (DBusWatch *watch,
               void      *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove */
  if (dbus_watch_get_enabled (watch))
    add_watch (watch, data);
  else
    remove_watch (watch, data);
}

static dbus_bool_t
add_timeout (DBusTimeout *timeout,
	     void        *data)
{
  DBusSource *source = data;
  
  if (!dbus_timeout_get_enabled (timeout))
    return TRUE;

  dbus_source_add_timeout (source, timeout);

  return TRUE;
}

static void
remove_timeout (DBusTimeout *timeout,
		void        *data)
{
  DBusSource *source = data;

  dbus_source_remove_timeout (source, timeout);
}

static void
timeout_toggled (DBusTimeout *timeout,
                 void        *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove
   */
  if (dbus_timeout_get_enabled (timeout))
    add_timeout (timeout, data);
  else
    remove_timeout (timeout, data);
}

static void
wakeup_main (void *data)
{
  g_main_context_wakeup (NULL);
}

static const GSourceFuncs dbus_source_funcs = {
  dbus_source_prepare,
  dbus_source_check,
  dbus_source_dispatch
};

/* Called when the connection dies or when we're unintegrating from mainloop */
static void
dbus_source_free (DBusSource *dbus_source)
{
  while (dbus_source->ios)
    {
      IOHandler *handler = dbus_source->ios->data;
      
      dbus_watch_set_data (handler->watch, NULL, NULL);
    }

  while (dbus_source->timeouts)
    {
      TimeoutHandler *handler = dbus_source->timeouts->data;
      
      dbus_timeout_set_data (handler->timeout, NULL, NULL);
    }

  /* Remove from mainloop */
  g_source_destroy ((GSource *)dbus_source);

  g_source_unref ((GSource *)dbus_source);
}

void
_g_dbus_connection_integrate_with_main  (DBusConnection *connection)
{
  DBusSource *dbus_source;

  g_once (&once_init_main_integration, main_integration_init, NULL);
  
  g_assert (connection != NULL);

  _g_dbus_connection_remove_from_main (connection);

  dbus_source = (DBusSource *)
    g_source_new ((GSourceFuncs*)&dbus_source_funcs,
		  sizeof (DBusSource));
  
  dbus_source->connection = connection;
  
  if (!dbus_connection_set_watch_functions (connection,
                                            add_watch,
                                            remove_watch,
                                            watch_toggled,
                                            dbus_source, NULL))
    _g_dbus_oom ();

  if (!dbus_connection_set_timeout_functions (connection,
                                              add_timeout,
                                              remove_timeout,
                                              timeout_toggled,
                                              dbus_source, NULL))
    _g_dbus_oom ();
    
  dbus_connection_set_wakeup_main_function (connection,
					    wakeup_main,
					    dbus_source, NULL);

  /* Owned by both connection and mainloop (until destroy) */
  g_source_attach ((GSource *)dbus_source, NULL);

  if (!dbus_connection_set_data (connection,
				 main_integration_data_slot,
				 dbus_source, (DBusFreeFunction)dbus_source_free))
    _g_dbus_oom ();
}

void
_g_dbus_connection_remove_from_main (DBusConnection *connection)
{
  g_once (&once_init_main_integration, main_integration_init, NULL);

  if (!dbus_connection_set_data (connection,
				 main_integration_data_slot,
				 NULL, NULL))
    _g_dbus_oom ();
}

void
_g_dbus_message_iter_copy (DBusMessageIter *dest,
			   DBusMessageIter *source)
{
  int type, element_type;
  
  while (dbus_message_iter_get_arg_type (source) != DBUS_TYPE_INVALID)
    {
      type = dbus_message_iter_get_arg_type (source);

      if (dbus_type_is_basic (type))
	{
	  dbus_uint64_t value;
	  dbus_message_iter_get_basic (source, &value);
	  dbus_message_iter_append_basic (dest, type, &value);
	}
      else if (type == DBUS_TYPE_ARRAY)
	{
	  DBusMessageIter source_array, dest_array;
	  void *value;
	  int n_elements;
	  char buf[2];
	  
	  element_type = dbus_message_iter_get_element_type (source);
	  if (dbus_type_is_fixed (element_type))
	    {
	      buf[0] = element_type;
	      buf[1] = '\0';
	      
	      dbus_message_iter_recurse (source, &source_array);
	      dbus_message_iter_get_fixed_array (&source_array, &value, &n_elements);

	      if (!dbus_message_iter_open_container (dest, DBUS_TYPE_ARRAY,
						     buf, &dest_array))
		_g_dbus_oom ();
	      
	      if (!dbus_message_iter_append_fixed_array (&dest_array,
							 element_type,
							 &value, n_elements))
		_g_dbus_oom ();
	      
	      if (!dbus_message_iter_close_container (dest, &dest_array))
		_g_dbus_oom ();
	    }
	  else
	    g_error ("Unsupported array type %c in _g_dbus_message_iter_copy", element_type);
	}
      else
	g_error ("Unsupported type %c in _g_dbus_message_iter_copy", type);

      dbus_message_iter_next (source);      
    }
  
}

typedef struct {
  GAsyncDBusCallback callback;
  gpointer user_data;
  GError *io_error;
  
  
  /* protected by async_call lock: */
  gboolean ran;  /* the pending_call reply handler ran */
  gboolean idle; /* we queued an idle */

  /* only used for idle */
  DBusPendingCall *pending;
} AsyncDBusCallData;

/* Lock to protect the data for working around racecondition
   between send_with_reply and pending_set_notify */
G_LOCK_DEFINE_STATIC(async_call);

static void
handle_async_reply (DBusPendingCall *pending,
		    AsyncDBusCallData *data)
{
  DBusMessage *reply;
  GError *error;
  
  reply = dbus_pending_call_steal_reply (pending);
  
  error = NULL;
  if (_g_error_from_message (reply, &error))
    {
      if (data->callback)
	data->callback (NULL, error, data->user_data);
      g_error_free (error);
    }
  else
    {
      if (data->callback)
	data->callback (reply, NULL, data->user_data);
    }

  dbus_message_unref (reply);
}

static void
async_call_reply (DBusPendingCall *pending,
		  void            *_data)
{
  AsyncDBusCallData *data = _data;

  G_LOCK (async_call);
  if (data->idle)
    return;
  data->ran = TRUE;
  G_UNLOCK (async_call);

  handle_async_reply (pending, data);
}

static gboolean
idle_async_callback (void *_data)
{
  AsyncDBusCallData *data = _data;
  handle_async_reply (data->pending, data);
  dbus_pending_call_unref (data->pending);
  return FALSE;
}

static gboolean
async_call_error_at_idle (gpointer _data)
{
  AsyncDBusCallData *data = _data;

  if (data->callback)
    data->callback (NULL, data->io_error, data->user_data);

  g_error_free (data->io_error);
  g_free (data);
  
  return FALSE;
}

void
_g_dbus_connection_call_async (DBusConnection *connection,
			       DBusMessage *message,
			       int timeout_msecs,
			       GAsyncDBusCallback callback,
			       gpointer user_data)
{
  AsyncDBusCallData *data;
  DBusPendingCall *pending_call;
  DBusError derror;

  data = g_new0 (AsyncDBusCallData, 1);
  data->callback = callback;
  data->user_data = user_data;
  
  if (connection == NULL)
    {
      dbus_error_init (&derror);
      connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
      if (connection == NULL)
	{
	  g_set_error_literal (&data->io_error, G_IO_ERROR, G_IO_ERROR_FAILED,
			       "Can't open dbus connection");
	  g_idle_add (async_call_error_at_idle, data);
          dbus_error_free (&derror);
	  return;
	}
    }

  if (!dbus_connection_send_with_reply (connection, message, &pending_call, timeout_msecs))
    _g_dbus_oom ();
  
  if (pending_call == NULL)
    {
      g_set_error (&data->io_error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_call_error_at_idle, data);
      return;
    }

  if (!dbus_pending_call_set_notify (pending_call,
				     async_call_reply,
				     data, g_free))
    _g_dbus_oom ();


  /* All this is required to work around a race condition between
   * send_with_reply and pending_call_set_notify :/
   */
  G_LOCK (async_call);

  if (dbus_pending_call_get_completed (pending_call) &&
      !data->ran)
    {
      data->idle = TRUE;
      data->pending = dbus_pending_call_ref (pending_call);
      g_idle_add (idle_async_callback, data);
    }
    
  G_UNLOCK (async_call);
    
  
  dbus_pending_call_unref (pending_call);
}
