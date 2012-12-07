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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <gvfsdaemonprotocol.h>
#include <gio/gio.h>

static const char *
get_object_signature (GObject *obj)
{
  if (G_IS_ICON (obj))
      return "(us)";
  return "(u)";
}

static GVariant *
append_object (GObject *obj)
{
  GVariant *var;

  /* version 1 and 2 are deprecated old themed-icon and file-icon values */
  if (G_IS_ICON (obj))
    {
      char *data;

      data = g_icon_to_string (G_ICON (obj));
      var = g_variant_new ("(us)", 3, data);
      g_free (data);
    }
  else
    {
      /* NULL or unknown type: */
      if (obj != NULL)
	g_warning ("Unknown attribute object type, ignoring");
      
      var = g_variant_new ("(u)", 0);
    }

  return var;
}

void
_g_dbus_attribute_value_destroy (GFileAttributeType          type,
				 GDBusAttributeValue        *value)
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
			      GDBusAttributeValue *value)
{
  switch (type) {
  case G_FILE_ATTRIBUTE_TYPE_STRING:
  case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
  case G_FILE_ATTRIBUTE_TYPE_OBJECT:
  case G_FILE_ATTRIBUTE_TYPE_STRINGV:
    return value->ptr;
  default:
    return (gpointer) value;
  }
}

static const char *
_g_dbus_type_from_file_attribute_type (GFileAttributeType type)
{
  const char *dbus_type;

  switch (type)
    {
    case G_FILE_ATTRIBUTE_TYPE_STRING:
      dbus_type = "s";
      break;
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      dbus_type = "ay";
      break;
    case G_FILE_ATTRIBUTE_TYPE_STRINGV:
      dbus_type = "as";
      break;
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      dbus_type = "b";
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      dbus_type = "u";
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      dbus_type = "i";
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      dbus_type = "t";
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      dbus_type = "x";
      break;
    case G_FILE_ATTRIBUTE_TYPE_OBJECT:
      dbus_type = "r";
      break;
    case G_FILE_ATTRIBUTE_TYPE_INVALID:
      dbus_type = "ay";
      break;
    default:
      dbus_type = NULL;
      g_warning ("Invalid attribute type %u, ignoring\n", type);
      break;
    }

  return dbus_type;
}

GVariant *
_g_dbus_append_file_attribute (const char *attribute,
			       GFileAttributeStatus status,
			       GFileAttributeType type,
			       gpointer value_p)
{
  const char *dbus_type;
  GVariant *v;

  dbus_type = _g_dbus_type_from_file_attribute_type (type);

  if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_TUPLE))
    dbus_type = get_object_signature ((GObject *)value_p);

  if (g_variant_type_is_tuple (G_VARIANT_TYPE (dbus_type)))
    v = append_object ((GObject *)value_p);
  else if (g_variant_type_is_array (G_VARIANT_TYPE (dbus_type)))
    {
      char *s;
      
      s = g_strdup_printf ("^%s", dbus_type);
      v = g_variant_new (s, value_p);
      g_free (s);
    }
  else if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_UINT32))
    v = g_variant_new (dbus_type, *(guint32 *)value_p);
  else if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_INT32))
    v = g_variant_new (dbus_type, *(gint32 *)value_p);
  else if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_UINT64))
    v = g_variant_new (dbus_type, *(guint64 *)value_p);
  else if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_INT64))
    v = g_variant_new (dbus_type, *(gint64 *)value_p);
  else if (g_variant_type_equal (G_VARIANT_TYPE (dbus_type), G_VARIANT_TYPE_BOOLEAN))
    v = g_variant_new (dbus_type, *(gboolean *)value_p);
  else
    v = g_variant_new (dbus_type, value_p);
  
  return g_variant_new ("(suv)",
                        attribute,
                        status,
                        v);
}

GVariant *
_g_dbus_append_file_info (GFileInfo *info)
{
  GVariantBuilder builder;
  char **attributes;
  int i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(suv)"));

  attributes = g_file_info_list_attributes (info, NULL);
  for (i = 0; attributes[i] != NULL; i++)
    {
      GFileAttributeType type;
      GFileAttributeStatus status;
      gpointer value_p;

      if (g_file_info_get_attribute_data (info, attributes[i], &type, &value_p, &status))
        g_variant_builder_add_value (&builder, 
            _g_dbus_append_file_attribute (attributes[i], status, type, value_p));
    }
  g_strfreev (attributes);

  return g_variant_builder_end (&builder);
}

gboolean
_g_dbus_get_file_attribute (GVariant *value,
			    gchar **attribute,
			    GFileAttributeStatus *status,
			    GFileAttributeType *type,
			    GDBusAttributeValue *attr_value)
{
  gboolean res;
  char *str;
  guint32 obj_type;
  GObject *obj;
  GVariant *v;

  g_variant_get (value, "(suv)",
                 attribute,
                 status,
                 &v);

  res = TRUE;
  if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_STRING;
      g_variant_get (v, "s", &attr_value->ptr);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_BYTESTRING))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_BYTE_STRING;
      g_variant_get (v, "^ay", &attr_value->ptr);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING_ARRAY))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_STRINGV;
      g_variant_get (v, "^as", &attr_value->ptr);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_BYTE))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_INVALID;
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_BOOLEAN;
      g_variant_get (v, "b", &attr_value->boolean);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_UINT32;
      g_variant_get (v, "u", &attr_value->uint32);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT32))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_INT32;
      g_variant_get (v, "i", &attr_value->ptr);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT64))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_UINT64;
      g_variant_get (v, "t", &attr_value->uint64);
    } 
  else if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT64))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_INT64;
      g_variant_get (v, "x", &attr_value->ptr);
    } 
  else if (g_variant_is_container (v))
    {
      *type = G_FILE_ATTRIBUTE_TYPE_OBJECT;
      obj_type = G_MAXUINT32;   /* treat it as an error if not set below */
      str = NULL;

      if (g_variant_is_of_type (v, G_VARIANT_TYPE ("(u)")))
        {
          g_variant_get (v, "(u)", &obj_type);
        }
      else if (g_variant_is_of_type (v, G_VARIANT_TYPE ("(us)")))
        {
          g_variant_get (v, "(u&s)", &obj_type, &str);
        }
      
      obj = NULL;

      /* obj_type 1 and 2 are deprecated and treated as errors */
      if (obj_type == 3)
        {
          if (str != NULL)
            {
              /* serialized G_ICON */
              obj = (GObject *)g_icon_new_for_string (str, NULL);
            }
          else
            {
              g_warning ("Malformed object data in file attribute");
            }
        }
      else
        {
          /* NULL (or unsupported) */
          if (obj_type != 0)
            g_warning ("Unsupported object type in file attribute");
        }
      attr_value->ptr = obj;
    } 
  else
    res = FALSE;

  g_variant_unref (v);
  
  return res;
}

GFileInfo *
_g_dbus_get_file_info (GVariant *value,
		       GError **error)
{
  GFileInfo *info;
  gchar *attribute;
  GFileAttributeType type;
  GFileAttributeStatus status;
  GDBusAttributeValue attr_value;
  GVariantIter iter;
  GVariant *child;

  info = g_file_info_new ();

  g_variant_iter_init (&iter, value);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      if (!_g_dbus_get_file_attribute (child, &attribute, &status, &type, &attr_value))
        goto error;

      g_file_info_set_attribute (info, attribute, type, _g_dbus_attribute_as_pointer (type, &attr_value));
      if (status)
        g_file_info_set_attribute_status (info, attribute, status);

      g_free (attribute);
      _g_dbus_attribute_value_destroy (type, &attr_value);

      g_variant_unref (child);
    }

  return info;

 error:
  g_object_unref (info);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("Invalid file info format"));
  return NULL;
}

GFileAttributeInfoList *
_g_dbus_get_attribute_info_list (GVariant *value,
				 GError **error)
{
  GFileAttributeInfoList *list;
  GVariantIter iter;
  const char *name;
  guint32 type, flags;
  
  list = g_file_attribute_info_list_new ();

  g_variant_iter_init (&iter, value);
  while (g_variant_iter_next (&iter, "(&suu)", &name, &type, &flags))
    g_file_attribute_info_list_add (list, name, type, flags);
  
  return list;
}

GVariant *
_g_dbus_append_attribute_info_list (GFileAttributeInfoList *list)
{
  GVariantBuilder builder;
  int i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(suu)"));

  for (i = 0; i < list->n_infos; i++)
    g_variant_builder_add (&builder, "(suu)", 
                                     list->infos[i].name,
                                     list->infos[i].type,
                                     list->infos[i].flags);
  
  return g_variant_builder_end (&builder);
}
