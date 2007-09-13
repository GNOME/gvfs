#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <gdbusutils.h>

static void oom (void) G_GNUC_NORETURN;

static void oom (void)
{
  g_error ("DBus failed with out of memory error");
  exit(1);
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

char *
_g_dbus_unescape_bus_name (const char *escaped, const char *end)
{
  GString *s = g_string_new ("");
  
  if (end == NULL)
    end = escaped + strlen (escaped);

  append_unescaped_dbus_name (s, escaped, end);
  return g_string_free (s, FALSE);
}

/* We use _ for escaping */
#define VALID_INITIAL_BUS_NAME_CHARACTER(c)     \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_') || */((c) == '-'))
#define VALID_BUS_NAME_CHARACTER(c)             \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_')||*/  ((c) == '-'))

void
_g_dbus_append_escaped_bus_name (GString *s,
				 gboolean at_start,
				 const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first && at_start)
	{
	  if (VALID_INITIAL_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_BUS_NAME_CHARACTER (c))
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

void
_g_dbus_message_iter_append_cstring (DBusMessageIter *iter, const char *str)
{
  DBusMessageIter array;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE_AS_STRING,
					 &array))
    oom ();
  
  if (!dbus_message_iter_append_fixed_array (&array,
					     DBUS_TYPE_BYTE,
					     &str, strlen (str)))
    oom ();
  
  if (!dbus_message_iter_close_container (iter, &array))
    oom ();
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

  while (type != DBUS_TYPE_INVALID)
    {
      if (type == G_DBUS_TYPE_CSTRING)
	{
	  const char **value_p;
	  const char *value;

	  value_p = va_arg (var_args, const char**);
	  value = *value_p;

	  _g_dbus_message_iter_append_cstring (&iter, value);
	}
      else if (dbus_type_is_basic (type))
        {
          const void *value;
          value = va_arg (var_args, const void*);

          if (!dbus_message_iter_append_basic (&iter,
                                               type,
                                               value))
	    oom ();
        }
      else if (type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          DBusMessageIter array;
          char buf[2];

          element_type = va_arg (var_args, int);
              
          buf[0] = element_type;
          buf[1] = '\0';
          if (!dbus_message_iter_open_container (&iter,
                                                 DBUS_TYPE_ARRAY,
                                                 buf,
                                                 &array))
	    oom ();
          
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
		oom ();
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
		    oom ();
                  ++i;
                }
            }
          else
            {
              g_error ("arrays of %d can't be appended with _g_dbus_message_append_args_valist for now\n",
		       element_type);
            }

          if (!dbus_message_iter_close_container (&iter, &array))
	    oom ();
        }

      type = va_arg (var_args, int);
    }
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
_g_error_from_dbus (DBusError *derror, 
		    GError **error)
{
  const char *name, *end;;
  char *m;
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
      
      g_set_error (error, domain, code, "%s", derror->message);
    }
  /* TODO: Special case other types, like DBUS_ERROR_NO_MEMORY etc? */
  else
    {
      m = g_strdup_printf ("DBus error %s: %s", derror->name, derror->message);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO, "%s", m);
      g_free (m);
    }
}
