#include <glib-object.h>
#include <dbus/dbus.h>
#include <gvfsdaemonprotocol.h>
#include <gdbusutils.h>
#include <gio/gthemedicon.h>
#include <gio/gfileicon.h>
#include <gio/glocalfile.h>

gchar *
g_dbus_type_from_file_attribute_type (GFileAttributeType type)
{
  char *dbus_type;

  switch (type)
    {
    case G_FILE_ATTRIBUTE_TYPE_STRING:
      dbus_type = DBUS_TYPE_STRING_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      dbus_type = DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      dbus_type = DBUS_TYPE_UINT32_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      dbus_type = DBUS_TYPE_INT32_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      dbus_type = DBUS_TYPE_UINT64_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      dbus_type = DBUS_TYPE_INT64_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_OBJECT:
      dbus_type = DBUS_TYPE_STRUCT_AS_STRING;
      break;
    default:
      dbus_type = NULL;
      g_warning ("Invalid attribute type %d, ignoring\n", type);
      break;
    }

  return dbus_type;
}

static void
append_file_attribute (DBusMessageIter *iter,
		       const char *attribute,
		       GFileAttributeType type,
		       gconstpointer value)
{
  DBusMessageIter struct_iter, variant_iter, array_iter, inner_struct_iter, obj_struct_iter;
  char *dbus_type;
  guint32 v_uint32;
  gint32 v_int32;
  guint64 v_uint64;
  gint64 v_int64;
  const char *str = NULL;
  GObject *obj = NULL;

  dbus_type = g_dbus_type_from_file_attribute_type (type);

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 DBUS_TYPE_STRING_AS_STRING
					 DBUS_TYPE_VARIANT_AS_STRING,
					 &inner_struct_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&inner_struct_iter,
				       DBUS_TYPE_STRING,
				       &attribute))
    _g_dbus_oom ();


  if (!dbus_message_iter_open_container (&inner_struct_iter,
					 DBUS_TYPE_VARIANT,
					 dbus_type,
					 &variant_iter))
    _g_dbus_oom ();

  if (dbus_type[0] == DBUS_TYPE_ARRAY)
    _g_dbus_message_iter_append_cstring (&variant_iter, *(char **)value);
  else if (dbus_type[0] == DBUS_TYPE_STRUCT)
    {
      obj = (GObject *) value;

      if (G_IS_THEMED_ICON (obj))
	{
	  char **icons;

	  icons = g_themed_icon_get_names (G_THEMED_ICON (obj));
	  v_uint32 = 1;
	  if (!dbus_message_iter_append_basic (&obj_struct_iter,
					       DBUS_TYPE_UINT32, &v_uint32))
	    _g_dbus_oom ();

	  if (!dbus_message_iter_open_container (&variant_iter,
						 DBUS_TYPE_STRUCT,
						 DBUS_TYPE_UINT32_AS_STRING
						 DBUS_TYPE_ARRAY_AS_STRING
						 DBUS_TYPE_STRING_AS_STRING,
						 &obj_struct_iter))
	    _g_dbus_oom ();

	  if (!dbus_message_iter_append_fixed_array (&obj_struct_iter,
						     DBUS_TYPE_STRING,
						     icons,
						     g_strv_length (icons)))
	    _g_dbus_oom ();
	}
      else if (G_IS_FILE_ICON (obj))
	{
	  GFile *file;
	  char *path;

	  file = g_file_icon_get_file (G_FILE_ICON (obj));

	  if (G_IS_LOCAL_FILE (file))
	    {
	      if (!dbus_message_iter_open_container (&variant_iter,
						     DBUS_TYPE_STRUCT,
						     DBUS_TYPE_UINT32_AS_STRING
						     DBUS_TYPE_ARRAY_AS_STRING
						     DBUS_TYPE_BYTE_AS_STRING,
						     &obj_struct_iter))
		_g_dbus_oom ();
		    

	      v_uint32 = 2;
	      if (!dbus_message_iter_append_basic (&obj_struct_iter,
						   DBUS_TYPE_UINT32, &v_uint32))
		_g_dbus_oom ();
		  
	      path = g_file_get_path (file);
	      _g_dbus_message_iter_append_cstring (&obj_struct_iter, path);
	      g_free (path);
	    }
	  else
	    {
	      /* Seems unlikely that daemon backend will generate GFileIcons with
		 files on the vfs, so its probably not a problem not to support this.
		 (Its tricky to support, since we don't link the daemon to the client/
		 library directly.) */
	      g_warning ("Unknown file type for icon in %s, ignoring", attribute);

	      if (!dbus_message_iter_open_container (&variant_iter,
						     DBUS_TYPE_STRUCT,
						     DBUS_TYPE_UINT32_AS_STRING,
						     &obj_struct_iter))
		_g_dbus_oom ();
		  
	      v_uint32 = 0;
	      if (!dbus_message_iter_append_basic (&obj_struct_iter,
						   DBUS_TYPE_UINT32, &v_uint32))
		_g_dbus_oom ();
	    }
	  g_object_unref (file);
	}
      else
	{
	  /* NULL or unknown type: */
	  if (obj != NULL)
	    g_warning ("Unknown attribute object type for %s, ignoring", attribute);

	  if (!dbus_message_iter_open_container (&variant_iter,
						 DBUS_TYPE_STRUCT,
						 DBUS_TYPE_UINT32_AS_STRING,
						 &obj_struct_iter))
	    _g_dbus_oom ();
	      
	  v_uint32 = 0;
	  if (!dbus_message_iter_append_basic (&obj_struct_iter,
					       DBUS_TYPE_UINT32, &v_uint32))
	    _g_dbus_oom ();
	}
	  
      if (!dbus_message_iter_close_container (&variant_iter, &obj_struct_iter))
	_g_dbus_oom ();
    }
  else
    {
      if (!dbus_message_iter_append_basic (&variant_iter,
					   dbus_type[0], value))
	_g_dbus_oom ();
    }

  if (obj)
    g_object_unref (obj);
      
  if (!dbus_message_iter_close_container (&inner_struct_iter, &variant_iter))
    _g_dbus_oom ();
      
  if (!dbus_message_iter_close_container (iter, &inner_struct_iter))
    _g_dbus_oom ();
}

void
g_dbus_append_file_attribute (DBusMessageIter *iter,
			      const char *attribute,
			      GFileAttributeType type,
			      gconstpointer value)
{
  append_file_attribute (iter, attribute, type, value);
}

void
g_dbus_append_file_info (DBusMessageIter *iter,
			 GFileInfo *info)
{
  DBusMessageIter struct_iter, array_iter;
  char **attributes;
  int i;

  attributes = g_file_info_list_attributes (info, NULL);

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 G_FILE_INFO_INNER_TYPE_AS_STRING,
					 &struct_iter))
    _g_dbus_oom ();


  if (!dbus_message_iter_open_container (&struct_iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					 DBUS_TYPE_STRING_AS_STRING	
					 DBUS_TYPE_VARIANT_AS_STRING	
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (i = 0; attributes[i] != NULL; i++)
    {
      GFileAttributeType type;
      char *dbus_type;
      void *value;
      const char *str;
      guint32 v_uint32;
      gint32 v_int32;
      guint64 v_uint64;
      gint64 v_int64;
      GObject *obj;

      value = NULL;
      obj = NULL;
      type = g_file_info_get_attribute_type (info, attributes[i]);
      switch (type)
	{
	case G_FILE_ATTRIBUTE_TYPE_STRING:
	  dbus_type = DBUS_TYPE_STRING_AS_STRING;
	  str = g_file_info_get_attribute_string (info, attributes[i]);
	  value = &str;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
	  dbus_type = DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING;
	  str = g_file_info_get_attribute_byte_string (info, attributes[i]);
	  value = &str;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT32:
	  dbus_type = DBUS_TYPE_UINT32_AS_STRING;
	  v_uint32 = g_file_info_get_attribute_uint32 (info, attributes[i]);
	  value = &v_uint32;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT32:
	  dbus_type = DBUS_TYPE_INT32_AS_STRING;
	  v_int32 = g_file_info_get_attribute_int32 (info, attributes[i]);
	  value = &v_int32;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT64:
	  dbus_type = DBUS_TYPE_UINT64_AS_STRING;
	  v_uint64 = g_file_info_get_attribute_uint64 (info, attributes[i]);
	  value = &v_uint64;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT64:
	  dbus_type = DBUS_TYPE_INT64_AS_STRING;
	  v_int64 = g_file_info_get_attribute_int64 (info, attributes[i]);
	  value = &v_int64;
	  break;
	case G_FILE_ATTRIBUTE_TYPE_OBJECT:
	  dbus_type = DBUS_TYPE_STRUCT_AS_STRING;
	  obj = g_file_info_get_attribute_object (info, attributes[i]);
	  value = obj;
	  break;
	default:
	  g_warning ("Invalid attribute type %s=%d, ignoring\n", attributes[i], type);
	  continue;
	}

      append_file_attribute (&array_iter, attributes [i], type, value);
    }

  g_strfreev (attributes);

  if (!dbus_message_iter_close_container (&struct_iter, &array_iter))
    _g_dbus_oom ();
      
  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}
