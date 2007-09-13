#include <glib-object.h>
#include <dbus/dbus.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>

char *
g_dbus_get_file_info_signature (GFileInfoRequestFlags requested)
{
  GString *str;

  str = g_string_new (DBUS_STRUCT_BEGIN_CHAR_AS_STRING);

  
  if (requested & G_FILE_INFO_FILE_TYPE)
    {
      g_string_append_c (str, DBUS_TYPE_UINT16);
    }

  if (requested & G_FILE_INFO_NAME)
    {
      g_string_append (str, DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING);
    }

  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      g_string_append_c (str, DBUS_TYPE_STRING);
    }

  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      g_string_append_c (str, DBUS_TYPE_STRING);
    }

  if (requested & G_FILE_INFO_ICON)
    {
      g_string_append_c (str, DBUS_TYPE_STRING);
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      g_string_append_c (str, DBUS_TYPE_STRING);
    }

  if (requested & G_FILE_INFO_SIZE)
    {
      g_string_append_c (str, DBUS_TYPE_UINT64);
    }

  if (requested & G_FILE_INFO_MODIFICATION_TIME)
    {
      g_string_append_c (str, DBUS_TYPE_UINT64);
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      g_string_append_c (str, DBUS_TYPE_UINT32);
    }

  if (requested & G_FILE_INFO_STAT_INFO)
    {
      g_string_append_c (str, DBUS_TYPE_UINT32); /* TODO: struct */
    }

  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      g_string_append (str, DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING);
    }

  if (requested & G_FILE_INFO_IS_HIDDEN)
    {
      g_string_append_c (str, DBUS_TYPE_BOOLEAN);
    }
  

  /* TODO: Attributes */
  
  g_string_append_c (str, DBUS_STRUCT_END_CHAR);

  return g_string_free (str, FALSE);
}

void
g_dbus_append_file_info (DBusMessageIter *iter,
			 GFileInfoRequestFlags requested,
			 GFileInfo *info)
{
  DBusMessageIter struct_iter;
  char *sig;

  sig = g_dbus_get_file_info_signature (requested);
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 sig,
					 &struct_iter))
    g_dbus_oom ();
  g_free (sig);

  if (requested & G_FILE_INFO_FILE_TYPE)
    {
      guint16 type = g_file_info_get_file_type (info);
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT16,
					   &type))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_NAME)
    {
      _g_dbus_message_iter_append_cstring (&struct_iter,
					   g_file_info_get_name (info));
    }

  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      const char *str = g_file_info_get_display_name (info);
      
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_STRING,
					   &str))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      const char *str = g_file_info_get_edit_name (info);
      
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_STRING,
					   &str))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_ICON)
    {
      const char *str = g_file_info_get_icon (info);
      
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_STRING,
					   &str))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      const char *str = g_file_info_get_mime_type (info);
      
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_STRING,
					   &str))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_SIZE)
    {
      guint64 size = g_file_info_get_size (info);
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT64,
					   &size))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_MODIFICATION_TIME)
    {
      guint64 time = g_file_info_get_modification_time (info);
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT64,
					   &time))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      guint32 rights = g_file_info_get_access_rights (info);
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT32,
					   &rights))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_STAT_INFO)
    {
      /* TODO: implement statinfo */
      guint32 tmp = 0;
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT32,
					   &tmp))
	g_dbus_oom ();
    }

  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      _g_dbus_message_iter_append_cstring (&struct_iter,
					   g_file_info_get_symlink_target (info));
    }

  if (requested & G_FILE_INFO_IS_HIDDEN)
    {
      dbus_bool_t is_hidden = g_file_info_get_is_hidden (info);
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_BOOLEAN,
					   &is_hidden))
	g_dbus_oom ();
    }

  /* TODO: Attributes */

  if (!dbus_message_iter_close_container (iter, &struct_iter))
    g_dbus_oom ();
}
