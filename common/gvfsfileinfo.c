/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2009 Red Hat, Inc.
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

#include <gio/gio.h>
#include <string.h>
#include "gvfsfileinfo.h"

static void
put_string (GDataOutputStream *out,
	    const char *str)
{
  gsize len;

  len = strlen (str);
  if (len > G_MAXUINT16)
    {
      g_warning ("GFileInfo string to large, (%d bytes)\n", (int)len);
      len = 0;
      str = "";
    }
  
  g_data_output_stream_put_uint16 (out, len,
				   NULL, NULL);
  g_data_output_stream_put_string (out, str, NULL, NULL);
}

static void
put_stringv (GDataOutputStream *out,
	     char **strv)
{
  int len, i;

  len = g_strv_length (strv);
  if (len > G_MAXUINT16)
    {
      g_warning ("GFileInfo stringv to large, (%d elements)\n", (int)len);
      len = 0;
    }

  g_data_output_stream_put_uint16 (out, len,
				   NULL, NULL);
  for (i = 0; i < len; i++)
    put_string (out, strv[i]);
}

static char *
read_string (GDataInputStream *in)
{
  gsize len;
  char *str;

  len = g_data_input_stream_read_uint16 (in, NULL, NULL);
  str = g_malloc (len + 1);
  g_input_stream_read_all (G_INPUT_STREAM (in), str, len, &len, NULL, NULL);
  str[len] = 0;
  return str;
}

static char **
read_stringv (GDataInputStream *in)
{
  int len, i;
  char **strv;

  len = g_data_input_stream_read_uint16 (in, NULL, NULL);

  strv = g_new (char *, len + 1);
  for (i = 0; i < len; i++)
    strv[i] = read_string (in);
  strv[i] = NULL;
  return strv;
}

char *
gvfs_file_info_marshal (GFileInfo *info,
			gsize     *size)
{
  GOutputStream *memstream;
  GDataOutputStream *out;
  GFileAttributeType type;
  GFileAttributeStatus status;
  GObject *obj;
  char **attrs, *attr;
  char *data;
  int i;

  memstream = g_memory_output_stream_new (NULL, 0, g_realloc, NULL);

  out = g_data_output_stream_new (memstream);
  g_object_unref (memstream);

  attrs = g_file_info_list_attributes (info, NULL);

  g_data_output_stream_put_uint32 (out,
				   g_strv_length (attrs),
				   NULL, NULL);

  for (i = 0; attrs[i] != NULL; i++)
    {
      attr = attrs[i];

      type = g_file_info_get_attribute_type  (info, attr);
      status = g_file_info_get_attribute_status  (info, attr);
      
      put_string (out, attr);
      g_data_output_stream_put_byte (out, type, 
				     NULL, NULL);
      g_data_output_stream_put_byte (out, status, 
				     NULL, NULL);

      switch (type)
	{
	case G_FILE_ATTRIBUTE_TYPE_STRING:
	  put_string (out, g_file_info_get_attribute_string (info, attr));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
	  put_string (out, g_file_info_get_attribute_byte_string (info, attr));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_STRINGV:
	  put_stringv (out, g_file_info_get_attribute_stringv (info, attr));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
	  g_data_output_stream_put_byte (out,
					 g_file_info_get_attribute_boolean (info, attr),
					 NULL, NULL);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT32:
	  g_data_output_stream_put_uint32 (out,
					   g_file_info_get_attribute_uint32 (info, attr),
					  NULL, NULL);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT32:
	  g_data_output_stream_put_int32 (out,
					  g_file_info_get_attribute_int32 (info, attr),
					  NULL, NULL);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT64:
	  g_data_output_stream_put_uint64 (out,
					   g_file_info_get_attribute_uint64 (info, attr),
					  NULL, NULL);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT64:
	  g_data_output_stream_put_int64 (out,
					  g_file_info_get_attribute_int64 (info, attr),
					  NULL, NULL);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_OBJECT:
          obj = g_file_info_get_attribute_object (info, attr);
	  if (obj == NULL)
	    {
	      g_data_output_stream_put_byte (out, 0,
					     NULL, NULL);
	    }
	  else if (G_IS_ICON (obj))
	    {
	      char *icon_str;

	      icon_str = g_icon_to_string (G_ICON (obj));
	      g_data_output_stream_put_byte (out, 1,
					     NULL, NULL);
	      put_string (out, icon_str);
	      g_free (icon_str);
	    }
	  else
	    {
	      g_warning ("Unsupported GFileInfo object type %s\n",
			 g_type_name_from_instance ((GTypeInstance *)obj));
	      g_data_output_stream_put_byte (out, 0,
					     NULL, NULL);
	    }
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INVALID:
	default:
	  break;
	}
    }

  data = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (memstream));
  *size = g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (memstream));
  g_object_unref (out);
  g_strfreev (attrs);
  return data;
}

GFileInfo *
gvfs_file_info_demarshal (char      *data,
			  gsize      size)
{
  guint32 num_attrs, i;
  GInputStream *memstream;
  GDataInputStream *in;
  GFileInfo *info;
  char *attr, *str, **strv;
  GFileAttributeType type;
  GFileAttributeStatus status;
  GObject *obj;
  int objtype;

  memstream = g_memory_input_stream_new_from_data (data, size, NULL);
  in = g_data_input_stream_new (memstream);
  g_object_unref (memstream);

  info = g_file_info_new ();
  num_attrs = g_data_input_stream_read_uint32 (in, NULL, NULL);

  for (i = 0; i < num_attrs; i++)
    {
      attr = read_string (in);
      type = g_data_input_stream_read_byte (in, NULL, NULL);
      status = g_data_input_stream_read_byte (in, NULL, NULL);

      switch (type)
	{
	case G_FILE_ATTRIBUTE_TYPE_STRING:
	  str = read_string (in);
	  g_file_info_set_attribute_string (info, attr, str);
	  g_free (str);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
	  str = read_string (in);
	  g_file_info_set_attribute_byte_string (info, attr, str);
	  g_free (str);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_STRINGV:
	  strv = read_stringv (in);
	  g_file_info_set_attribute_stringv (info, attr, strv);
	  g_strfreev (strv);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
	  g_file_info_set_attribute_boolean (info, attr,
					     g_data_input_stream_read_byte (in,
									    NULL,
									    NULL));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT32:
	  g_file_info_set_attribute_uint32 (info, attr,
					    g_data_input_stream_read_uint32 (in,
									     NULL,
									     NULL));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT32:
	  g_file_info_set_attribute_int32 (info, attr,
					   g_data_input_stream_read_int32 (in,
									   NULL,
									   NULL));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_UINT64:
	  g_file_info_set_attribute_uint64 (info, attr,
					    g_data_input_stream_read_uint64 (in,
									     NULL,
									     NULL));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INT64:
	  g_file_info_set_attribute_int64 (info, attr,
					   g_data_input_stream_read_int64 (in,
									   NULL,
									   NULL));
	  break;
	case G_FILE_ATTRIBUTE_TYPE_OBJECT:
	  objtype = g_data_input_stream_read_byte (in, NULL, NULL);
	  obj = NULL;

	  if (objtype == 1)
	    {
	      char *icon_str;

	      icon_str = read_string (in);
	      obj = (GObject *)g_icon_new_for_string  (icon_str, NULL);
	      g_free (icon_str);
	    }
	  else
	    {
	      g_warning ("Unsupported GFileInfo object type %d\n", objtype);
	      g_free (attr);
	      goto out;
	    }
	  g_file_info_set_attribute_object (info, attr, obj);
	  if (obj)
	    g_object_unref (obj);
	  break;
	case G_FILE_ATTRIBUTE_TYPE_INVALID:
	  break;
	default:
	  g_warning ("Unsupported GFileInfo attribute type %d\n", type);
	  g_free (attr);
	  goto out;
	  break;
	}
      g_file_info_set_attribute_status (info, attr, status);
      g_free (attr);
    }
  
 out:
  g_object_unref (in);
  return info;
}


