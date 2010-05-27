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

#include <glib-object.h>
#include <dbus/dbus.h>
#include <glib/gi18n-lib.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdbusutils.h>
#include <gio/gio.h>

static const char *
get_object_signature (GObject *obj)
{
  if (G_IS_ICON (obj))
    {
      return
	DBUS_STRUCT_BEGIN_CHAR_AS_STRING
	 DBUS_TYPE_UINT32_AS_STRING
	 DBUS_TYPE_STRING_AS_STRING
	DBUS_STRUCT_END_CHAR_AS_STRING;
    }
  return
    DBUS_STRUCT_BEGIN_CHAR_AS_STRING
     DBUS_TYPE_UINT32_AS_STRING
    DBUS_STRUCT_END_CHAR_AS_STRING;
}

static void
append_strv (DBusMessageIter *iter, char **strv)
{
  _g_dbus_message_iter_append_args (iter,
				    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &strv, g_strv_length (strv),
				    0);
}

static void
append_object (DBusMessageIter *iter, GObject *obj)
{
  DBusMessageIter obj_struct_iter;
  guint32 v_uint32;
  
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &obj_struct_iter))
    _g_dbus_oom ();

  /* version 1 and 2 are deprecated old themed-icon and file-icon values */
  if (G_IS_ICON (obj))
    {
      char *data;

      data = g_icon_to_string (G_ICON (obj));
      v_uint32 = 3;
      if (!dbus_message_iter_append_basic (&obj_struct_iter,
					   DBUS_TYPE_UINT32, &v_uint32))
	_g_dbus_oom ();

      if (!dbus_message_iter_append_basic (&obj_struct_iter,
					   DBUS_TYPE_STRING, &data))
	_g_dbus_oom ();

      g_free (data);
    }
  else
    {
      /* NULL or unknown type: */
      if (obj != NULL)
	g_warning ("Unknown attribute object type, ignoring");
      
      v_uint32 = 0;
      if (!dbus_message_iter_append_basic (&obj_struct_iter,
					   DBUS_TYPE_UINT32, &v_uint32))
	_g_dbus_oom ();
    }
  
  if (!dbus_message_iter_close_container (iter, &obj_struct_iter))
    _g_dbus_oom ();
}

void
_g_dbus_attribute_value_destroy (GFileAttributeType          type,
				 GDbusAttributeValue        *value)
{
  switch (type) {
  case G_FILE_ATTRIBUTE_TYPE_STRING:
  case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
    g_free (value->ptr);
    break;
  case G_FILE_ATTRIBUTE_TYPE_STRINGV:
    g_strfreev (value->ptr);
    break;
  case G_FILE_ATTRIBUTE_TYPE_OBJECT:
    if (value->ptr)
      g_object_unref (value->ptr);
    break;
  default:
    break;
  }
}

gpointer
_g_dbus_attribute_as_pointer (GFileAttributeType type,
			      GDbusAttributeValue *value)
{
  switch (type) {
  case G_FILE_ATTRIBUTE_TYPE_STRING:
  case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
  case G_FILE_ATTRIBUTE_TYPE_OBJECT:
    return value->ptr;
  default:
    return (gpointer) value;
  }
}

const char *
_g_dbus_type_from_file_attribute_type (GFileAttributeType type)
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
    case G_FILE_ATTRIBUTE_TYPE_STRINGV:
      dbus_type = DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING;
      break;
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      dbus_type = DBUS_TYPE_BOOLEAN_AS_STRING;
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
    case G_FILE_ATTRIBUTE_TYPE_INVALID:
      dbus_type = DBUS_TYPE_BYTE_AS_STRING;
      break;
    default:
      dbus_type = NULL;
      g_warning ("Invalid attribute type %u, ignoring\n", type);
      break;
    }

  return dbus_type;
}

void
_g_dbus_append_file_attribute (DBusMessageIter *iter,
			       const char *attribute,
			       GFileAttributeStatus status,
			       GFileAttributeType type,
			       gpointer value_p)
{
  DBusMessageIter variant_iter, inner_struct_iter;
  const char *dbus_type;
  GObject *obj = NULL;
  guint32 dbus_status;

  dbus_type = _g_dbus_type_from_file_attribute_type (type);

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &inner_struct_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&inner_struct_iter,
				       DBUS_TYPE_STRING,
				       &attribute))
    _g_dbus_oom ();

  dbus_status = status;
  if (!dbus_message_iter_append_basic (&inner_struct_iter,
				       DBUS_TYPE_UINT32,
				       &dbus_status))
    _g_dbus_oom ();

  if (dbus_type[0] == DBUS_TYPE_STRUCT)
    dbus_type = get_object_signature ((GObject *)value_p);
  
  if (!dbus_message_iter_open_container (&inner_struct_iter,
					 DBUS_TYPE_VARIANT,
					 dbus_type,
					 &variant_iter))
    _g_dbus_oom ();

  if (dbus_type[0] == DBUS_TYPE_STRING)
    {
      if (!dbus_message_iter_append_basic (&variant_iter,
					   DBUS_TYPE_STRING, &value_p))
	_g_dbus_oom ();
    }
  else if (dbus_type[0] == DBUS_TYPE_ARRAY && dbus_type[1] == DBUS_TYPE_BYTE)
    _g_dbus_message_iter_append_cstring (&variant_iter, (char *)value_p);
  else if (dbus_type[0] == DBUS_TYPE_ARRAY && dbus_type[1] == DBUS_TYPE_STRING)
    append_strv (&variant_iter, (char **)value_p);
  else if (dbus_type[0] == DBUS_TYPE_BYTE)
    {
      char byte = 0;
      if (!dbus_message_iter_append_basic (&variant_iter,
					   DBUS_TYPE_BYTE, &byte))
	_g_dbus_oom ();
    }
  else if (dbus_type[0] == DBUS_STRUCT_BEGIN_CHAR)
    append_object (&variant_iter, (GObject *)value_p);
  else if (dbus_type[0] == DBUS_TYPE_BOOLEAN)
    {
      /* dbus bool is uint32, gboolean is just "int", convert */
      dbus_bool_t bool = *(gboolean *)value_p;
      if (!dbus_message_iter_append_basic (&variant_iter,
					   dbus_type[0], &bool))
	_g_dbus_oom ();
    }
  else
    {
      /* All other types have the same size as dbus types */
      if (!dbus_message_iter_append_basic (&variant_iter,
					   dbus_type[0], value_p))
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
_g_dbus_append_file_info (DBusMessageIter *iter,
			  GFileInfo *info)
{
  DBusMessageIter struct_iter, array_iter;
  char **attributes;
  int i;

  attributes = g_file_info_list_attributes (info, NULL);

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &struct_iter))
    _g_dbus_oom ();


  if (!dbus_message_iter_open_container (&struct_iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					 DBUS_TYPE_STRING_AS_STRING	
					 DBUS_TYPE_UINT32_AS_STRING
					 DBUS_TYPE_VARIANT_AS_STRING	
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (i = 0; attributes[i] != NULL; i++)
    {
      GFileAttributeType type;
      GFileAttributeStatus status;
      gpointer value_p;

      if (g_file_info_get_attribute_data (info, attributes[i], &type, &value_p, &status))
	_g_dbus_append_file_attribute (&array_iter, attributes [i], status, type, value_p);
    }
  
  g_strfreev (attributes);

  if (!dbus_message_iter_close_container (&struct_iter, &array_iter))
    _g_dbus_oom ();
      
  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}

gboolean
_g_dbus_get_file_attribute (DBusMessageIter *iter,
			    gchar **attribute,
			    GFileAttributeStatus *status,
			    GFileAttributeType *type,
			    GDbusAttributeValue *value)
{
  char *str;
  char **strs;
  int n_elements;
  DBusMessageIter inner_struct_iter, variant_iter, cstring_iter, obj_iter;
  const gchar *attribute_temp;
  int element_type;
  dbus_uint32_t obj_type, dbus_status;
  dbus_bool_t dbus_bool;
  guint8 byte;
  GObject *obj;

  dbus_message_iter_recurse (iter, &inner_struct_iter);

  if (dbus_message_iter_get_arg_type (&inner_struct_iter) != DBUS_TYPE_STRING)
    goto error;
	
  dbus_message_iter_get_basic (&inner_struct_iter, &attribute_temp);
  *attribute = g_strdup (attribute_temp);

  dbus_message_iter_next (&inner_struct_iter);

  dbus_message_iter_get_basic (&inner_struct_iter, &dbus_status);
  if (status)
    *status = dbus_status;

  dbus_message_iter_next (&inner_struct_iter);

  if (dbus_message_iter_get_arg_type (&inner_struct_iter) != DBUS_TYPE_VARIANT)
    goto error;

  dbus_message_iter_recurse (&inner_struct_iter, &variant_iter);

  switch (dbus_message_iter_get_arg_type (&variant_iter))
    {
    case DBUS_TYPE_STRING:
      *type = G_FILE_ATTRIBUTE_TYPE_STRING;
      dbus_message_iter_get_basic (&variant_iter, &str);
      value->ptr = g_strdup (str);
      break;
    case DBUS_TYPE_ARRAY:
      element_type = dbus_message_iter_get_element_type (&variant_iter);
      if (element_type == DBUS_TYPE_BYTE)
	{
	  *type = G_FILE_ATTRIBUTE_TYPE_BYTE_STRING;

	  dbus_message_iter_recurse (&variant_iter, &cstring_iter);
	  dbus_message_iter_get_fixed_array (&cstring_iter,
					     &str, &n_elements);
	  value->ptr = g_strndup (str, n_elements);
	}
      else if (element_type == DBUS_TYPE_STRING)
	{
	  char **strv;
	  int n_elements;
	  if (!_g_dbus_message_iter_get_args (iter, NULL,
					      DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &strv, &n_elements,
					      0))
	    goto error;
	  *type = G_FILE_ATTRIBUTE_TYPE_STRINGV;
	  value->ptr = strv;
	}
      else
	goto error;

      break;
    case DBUS_TYPE_BYTE:
      dbus_message_iter_get_basic (&variant_iter, &byte);
      *type = G_FILE_ATTRIBUTE_TYPE_INVALID;
      break;
    case DBUS_TYPE_BOOLEAN:
      dbus_message_iter_get_basic (&variant_iter, &dbus_bool);
      value->boolean = dbus_bool;
      *type = G_FILE_ATTRIBUTE_TYPE_BOOLEAN;
      break;
    case DBUS_TYPE_UINT32:
      dbus_message_iter_get_basic (&variant_iter, value);
      *type = G_FILE_ATTRIBUTE_TYPE_UINT32;
      break;
    case DBUS_TYPE_INT32:
      dbus_message_iter_get_basic (&variant_iter, value);
      *type = G_FILE_ATTRIBUTE_TYPE_INT32;
      break;
    case DBUS_TYPE_UINT64:
      dbus_message_iter_get_basic (&variant_iter, value);
      *type = G_FILE_ATTRIBUTE_TYPE_UINT64;
      break;
    case DBUS_TYPE_INT64:
      dbus_message_iter_get_basic (&variant_iter, value);
      *type = G_FILE_ATTRIBUTE_TYPE_INT64;
      break;
    case DBUS_TYPE_STRUCT:
      dbus_message_iter_recurse (&variant_iter, &obj_iter);
      if (dbus_message_iter_get_arg_type (&obj_iter) != DBUS_TYPE_UINT32)
	goto error;
      
      *type = G_FILE_ATTRIBUTE_TYPE_OBJECT;

      dbus_message_iter_get_basic (&obj_iter, &obj_type);
      obj = NULL;
      
      dbus_message_iter_next (&obj_iter);
      /* 0 == NULL */

      if (obj_type == 1)
	{
	  /* Old deprecated G_THEMED_ICON */
	  if (_g_dbus_message_iter_get_args (&obj_iter,
					     NULL,
					     DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
					     &strs, &n_elements, 0))
	    {
	      obj = G_OBJECT (g_themed_icon_new_from_names (strs, n_elements));
	      g_strfreev (strs);
	    }
	}
      else if (obj_type == 2)
	{
	  /* Old deprecated G_FILE_ICON, w/ local file */
	  if (_g_dbus_message_iter_get_args (&obj_iter,
					     NULL,
					     G_DBUS_TYPE_CSTRING, &str,
					     0))
	    {
	      GFile *file = g_file_new_for_path (str);
	      obj = G_OBJECT (g_file_icon_new (file));
	      g_free (str);
	    }
	}
      else if (obj_type == 3)
	{
	  /* serialized G_ICON */
	  if (_g_dbus_message_iter_get_args (&obj_iter,
					     NULL,
					     DBUS_TYPE_STRING, &str,
					     0))
	    obj = (GObject *)g_icon_new_for_string (str, NULL);
	}
      else
	{
	  /* NULL (or unsupported) */
	  if (obj_type != 0)
	    g_warning ("Unsupported object type in file attribute");
	}
      
      value->ptr = obj;
      break;
    default:
      goto error;
    }

  return TRUE;

 error:
  return FALSE;
}

GFileInfo *
_g_dbus_get_file_info (DBusMessageIter *iter,
		       GError **error)
{
  GFileInfo *info;
  DBusMessageIter struct_iter, array_iter;
  gchar *attribute;
  GFileAttributeType type;
  GFileAttributeStatus status;
  GDbusAttributeValue value;

  info = g_file_info_new ();

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    goto error;

  dbus_message_iter_recurse (iter, &struct_iter);

  if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_ARRAY)
    goto error;
  
  dbus_message_iter_recurse (&struct_iter, &array_iter);

  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
    {
      if (!_g_dbus_get_file_attribute (&array_iter, &attribute, &status, &type, &value))
        goto error;

      g_file_info_set_attribute (info, attribute, type, _g_dbus_attribute_as_pointer (type, &value));
      if (status)
	g_file_info_set_attribute_status (info, attribute, status);

      g_free (attribute);
      _g_dbus_attribute_value_destroy (type, &value);

      dbus_message_iter_next (&array_iter);
    }

  dbus_message_iter_next (iter);
  return info;

 error:
  g_object_unref (info);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("Invalid file info format"));
  dbus_message_iter_next (iter);
  return NULL;
}

GFileAttributeInfoList *
_g_dbus_get_attribute_info_list (DBusMessageIter *iter,
				 GError **error)
{
  GFileAttributeInfoList *list;
  DBusMessageIter array_iter, struct_iter;
  const char *name;
  dbus_uint32_t type, flags;
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_ARRAY ||
      dbus_message_iter_get_element_type (iter) != DBUS_TYPE_STRUCT)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid attribute info list content"));
      return NULL;
    }

  list = g_file_attribute_info_list_new ();

  dbus_message_iter_recurse (iter, &array_iter);
  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
    {
      dbus_message_iter_recurse (&array_iter, &struct_iter);

      if (dbus_message_iter_get_arg_type (&struct_iter) == DBUS_TYPE_STRING)
	{
	  dbus_message_iter_get_basic (&struct_iter, &name);
	  dbus_message_iter_next (&struct_iter);
	  
	  if (dbus_message_iter_get_arg_type (&struct_iter) == DBUS_TYPE_UINT32)
	    {
	      dbus_message_iter_get_basic (&struct_iter, &type);
	      dbus_message_iter_next (&struct_iter);
	      
	      if (dbus_message_iter_get_arg_type (&struct_iter) == DBUS_TYPE_UINT32)
		{
		  dbus_message_iter_get_basic (&struct_iter, &flags);
		  
		  g_file_attribute_info_list_add (list, name, type, flags);
		}
	    }
	}
  
      dbus_message_iter_next (&array_iter);
    }

  return list;
}

void
_g_dbus_append_attribute_info_list (DBusMessageIter         *iter,
				    GFileAttributeInfoList  *list)
{
  DBusMessageIter array_iter, struct_iter;
  int i;
  dbus_uint32_t dbus_type, dbus_flags;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					 DBUS_TYPE_STRING_AS_STRING	
					 DBUS_TYPE_UINT32_AS_STRING
					 DBUS_TYPE_UINT32_AS_STRING	
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (i = 0; i < list->n_infos; i++)
    {
      if (!dbus_message_iter_open_container (&array_iter,
					     DBUS_TYPE_STRUCT,
					     NULL,
					     &struct_iter))
	_g_dbus_oom ();

      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_STRING, &list->infos[i].name))
	_g_dbus_oom ();
      
      dbus_type = list->infos[i].type;
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT32, &dbus_type))
	_g_dbus_oom ();
      
      dbus_flags = list->infos[i].flags;
      if (!dbus_message_iter_append_basic (&struct_iter,
					   DBUS_TYPE_UINT32, &dbus_flags))
	_g_dbus_oom ();
      
      if (!dbus_message_iter_close_container (&array_iter, &struct_iter))
	_g_dbus_oom ();
    }
  
  if (!dbus_message_iter_close_container (iter, &array_iter))
    _g_dbus_oom ();
}
