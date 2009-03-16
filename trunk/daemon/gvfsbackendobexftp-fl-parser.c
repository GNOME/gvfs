/*
 * Copyright (C) 2004-2005 Nokia Corporation.
 * Copyright (C) 2008 Bastien Nocera <hadess@hadess.net> (gio port)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <expat.h>

#include "gvfsbackendobexftp-fl-parser.h"

#define d(x)

typedef struct {
	GError **error;
	GList   *elements;

	gint     depth;
} ParserData;

/* Static functions declaration */
static void       fl_parser_start_node_cb     (void                *data,
					       const char          *el,
					       const char         **attr);
static void       fl_parser_end_node_cb       (void                *data,
					       const char          *el);
static XML_Parser fl_parser_create_context    (ParserData          *data);
static gboolean   fl_parser_fill_file_info    (GFileInfo           *file_info,
					       const char         **attr);
static void       fl_parser_free_parser_data  (ParserData          *data,
					       gboolean             free_list);


/* Function implementations */
static void 
fl_parser_start_node_cb (void        *user_data,
			 const char  *node_name,
			 const char **attr)
{
	ParserData *data;
	GFileInfo  *info;
	
	data = (ParserData *) user_data;
	
	data->depth++;
	
	d(g_print ("%d: %s\n", data->depth, node_name));

	if (data->depth > 2) {
		g_set_error (data->error,  
			     G_MARKUP_ERROR,  
			     G_MARKUP_ERROR_INVALID_CONTENT,  
			     "Don't expect node '%s' as child of 'file', 'folder' or 'parent-folder'",  
			     node_name); 
		return;
	}
	else if (data->depth == 1) {
		if (strcmp (node_name, "folder-listing") != 0) {
			g_set_error (data->error,  
				     G_MARKUP_ERROR,  
				     G_MARKUP_ERROR_INVALID_CONTENT,  
				     "Expected 'folder-listing', got '%s'",  
				     node_name);  
			return;
		}

		return;
	}

	if (strcmp (node_name, "parent-folder") == 0) {
		/* Just ignore parent-folder items */
		return;
	}
	
	info = g_file_info_new ();

	if (strcmp (node_name, "file") == 0) {
		g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
	}
	else if (strcmp (node_name, "folder") == 0) {
		GIcon *icon;
		g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
		g_file_info_set_content_type (info, "inode/directory");

		icon = g_themed_icon_new ("folder");
		g_file_info_set_icon (info, icon);
		g_object_unref (icon);
	} else {
		g_set_error (data->error,
			     G_MARKUP_ERROR,
			     G_MARKUP_ERROR_UNKNOWN_ELEMENT,
			     "Unknown element '%s'",
			     node_name);
		return;
	}

	if (!fl_parser_fill_file_info (info, attr)) {
		d(g_print ("Failed to fill GnomeVFSFileInfo from node '%s'\n",
			   node_name));
		g_object_unref (info);
		return;
	}

	if (g_file_info_get_content_type (info) == NULL) {
		char *mime_type;
		mime_type = g_content_type_guess (g_file_info_get_name (info), NULL, 0, NULL);
		g_file_info_set_content_type (info, mime_type);
		g_free (mime_type);
	}

	if (g_file_info_get_content_type (info) == NULL) {
		g_file_info_set_content_type (info, "application/octet-stream");
	}

	if (g_file_info_get_file_type (info) ==  G_FILE_TYPE_REGULAR) {
		GIcon *icon;

		icon = g_content_type_get_icon (g_file_info_get_content_type (info));
		if (icon != NULL) {
			if (G_IS_THEMED_ICON (icon))
				g_themed_icon_append_name (G_THEMED_ICON (icon), "text-x-generic");
			g_file_info_set_icon (info, icon);
			g_object_unref (icon);
		}
	}

	/* Permissions on folders in OBEX has different semantics than POSIX.
	 * In POSIX, if a folder is not writable, it means that it's content
	 * can't be removed, whereas in OBEX, it just means that the folder
	 * itself can't be removed. Therefore we must set all folders to RWD and
	 * handle the error when it happens. */
	if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
		g_file_info_set_attribute_boolean (info,
						   G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
						   TRUE);
		g_file_info_set_attribute_boolean (info,
						   G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
						   TRUE);
	}
	
	data->elements = g_list_prepend (data->elements, info);
}

static void
fl_parser_end_node_cb (void *user_data, const char *node_name)
{
	ParserData *data;

	data = (ParserData *) user_data;

	data->depth--;
	
	if (data->depth < 0) {  
		g_set_error (data->error,  
			     G_MARKUP_ERROR,  
			     G_MARKUP_ERROR_INVALID_CONTENT,  
			     "Closing non-open node '%s'",  
			     node_name);  
		return;  
	} 

	d(g_print ("%d: /%s\n", data->depth, node_name));
}

static XML_Parser
fl_parser_create_context (ParserData *data)
{
	XML_Parser parser;
	
	parser = XML_ParserCreate (NULL);
	
	XML_SetElementHandler(parser, 
			      (XML_StartElementHandler) fl_parser_start_node_cb,
			      (XML_EndElementHandler) fl_parser_end_node_cb);
	XML_SetUserData (parser, data);

	return parser;
}

static gboolean
fl_parser_fill_file_info (GFileInfo *info, const char **attr)
{
	gint i;
	
	for (i = 0; attr[i]; ++i) {
		const gchar *name;
		const gchar *value;

		name  = attr[i];
		value = attr[++i];
		
		if (strcmp (name, "name") == 0) {
			char *display_name;
			/* Apparently someone decided it was a good idea
			 * to send name="" mem-type="MMC" 
			 */
			if (!value || strcmp (value, "") == 0) {
				return FALSE;
			}

			g_file_info_set_name (info, value);
			display_name = g_filename_display_name (value);
			g_file_info_set_display_name (info, display_name);
			d(g_print ("Name: '%s'\n", display_name));
			g_free (display_name);
		}
		else if (strcmp (name, "size") == 0) {
			g_file_info_set_size (info, strtoll (value, NULL, 10));
			d(g_print ("Size: '%"G_GINT64_FORMAT"'\n", g_file_info_get_size (info)));
		}
		else if (strcmp (name, "modified") == 0) {
			GTimeVal time;

			if (g_time_val_from_iso8601 (value, &time) == FALSE)
				continue;
			g_file_info_set_modification_time (info, &time);
			d(g_print ("Modified: '%s' = '%d'\n", 
				   value, (int)time.tv_sec));
		}
		else if (strcmp (name, "created") == 0) {
			GTimeVal time;

			if (g_time_val_from_iso8601 (value, &time) == FALSE)
				continue;
			g_file_info_set_attribute_uint64 (info,
							  G_FILE_ATTRIBUTE_TIME_CREATED,
							  time.tv_sec);
			g_file_info_set_attribute_uint32 (info,
							  G_FILE_ATTRIBUTE_TIME_CREATED_USEC,
							  time.tv_usec);
			d(g_print ("Created: '%s' = '%d'\n", 
				   value, (int)time.tv_sec));
		}
		else if (strcmp (name, "accessed") == 0) {
			GTimeVal time;

			if (g_time_val_from_iso8601 (value, &time) == FALSE)
				continue;
			g_file_info_set_attribute_uint64 (info,
							  G_FILE_ATTRIBUTE_TIME_ACCESS,
							  time.tv_sec);
			g_file_info_set_attribute_uint32 (info,
							  G_FILE_ATTRIBUTE_TIME_ACCESS_USEC,
							  time.tv_usec);
			d(g_print ("Accessed: '%s' = '%d'\n", 
				   value, (int)time.tv_sec));
		}
		else if (strcmp (name, "user-perm") == 0) {
			/* The permissions don't map well to unix semantics,
			 * since the user is most likely not the same on both
			 * sides. We map the user permissions to "other" on the
			 * local side. D is treated as write, otherwise files
			 * can't be deleted through the module, even if it
			 * should be possible.
			 */
			if (strstr (value, "R")) {
				g_file_info_set_attribute_boolean (info,
								   G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
								   TRUE);
			}
			if (strstr (value, "W") || strstr (value, "D")) {
				g_file_info_set_attribute_boolean (info,
								   G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
								   TRUE);
			}
		}
		else if (strcmp (name, "group-perm") == 0) {
			/* Ignore for now */
			d(g_print ("Group permissions: '%s'\n", value));
		}
		else if (strcmp (name, "other-perm") == 0) {
			/* Ignore for now */
			d(g_print ("Other permissions: '%s'\n", value));
		}
		else if (strcmp (name, "owner") == 0) {
			/* Ignore for now */
			d(g_print ("Owner: '%s'\n", value));
		}
		else if (strcmp (name, "group") == 0) {
			/* Ignore for now */
			d(g_print ("Group: '%s'\n", value));
		}
		else if (strcmp (name, "type") == 0) {
			g_file_info_set_content_type (info, value);
			d(g_print ("Mime-Type: '%s'\n", value));
		}
		else if (strcmp (name, "xml:lang") == 0) {
			d(g_print ("Lang: '%s'\n", value));
		}
		else if (strcmp (name, "mem-type") == 0) {
			guint device;

			if (value == NULL || value[0] == '\0')
				continue;

			device = om_mem_type_id_from_string (value);
			g_file_info_set_attribute_uint32 (info,
							 G_FILE_ATTRIBUTE_UNIX_RDEV,
							 device);
			d(g_print ("Mem-Type: '%s' (%d)\n",
				   value, device));
		}
		else {
			d(g_print ("Unknown Attribute: %s = %s\n",
				   name, value));
		}
	}

	if (g_file_info_get_name (info) == NULL) { /* Required attribute */
		/* Set error */
		return FALSE;
	}
	
	return TRUE;
}

static void
fl_parser_free_parser_data (ParserData *data, gboolean free_list)
{
	if (free_list) {
		g_list_foreach (data->elements, (GFunc) g_object_unref, NULL);
		g_list_free (data->elements);
		data->elements = NULL;
	}

	g_free (data);
}

gboolean
gvfsbackendobexftp_fl_parser_parse (const gchar *buf, gint len, GList **elements,
				    GError **error)
{
	ParserData *data;
	XML_Parser  parser;

	data = g_new0 (ParserData, 1);
	data->error = error;
	data->elements = NULL;
	data->depth = 0;

	parser = fl_parser_create_context (data);
	if (!parser) {
		g_free (data);
		return FALSE;
	}

	if (XML_Parse (parser, buf, len, TRUE) == 0) {
		XML_ParserFree (parser);
		fl_parser_free_parser_data (data, TRUE);

		if (*error == NULL) {
			g_set_error_literal (error,
					     G_MARKUP_ERROR,
					     G_MARKUP_ERROR_INVALID_CONTENT,
					     "Couldn't parse the incoming data");
		}
		return FALSE;
	}

	XML_ParserFree (parser);
	
	*elements = data->elements;

	fl_parser_free_parser_data (data, FALSE);
		
	return TRUE;
}

static GPtrArray *mem_types = NULL;
static GHashTable *mem_types_ht = NULL;

guint
om_mem_type_id_from_string (const gchar *memtype)
{
	guint mem_id;
	gchar *value;

	if (memtype == NULL || memtype[0] == '\0')
		return 0;

	if (mem_types_ht != NULL) {
		mem_id = GPOINTER_TO_UINT (g_hash_table_lookup
					   (mem_types_ht, memtype));
		if (mem_id != 0)
			return mem_id;
	} else {
		mem_types = g_ptr_array_new ();
		/* Insert a dummy entry, so that we don't use 0 as a mem_id */
		g_ptr_array_add (mem_types, NULL);
		mem_types_ht = g_hash_table_new (g_str_hash, g_str_equal);
	}
	value = g_strdup (memtype);
	mem_id = mem_types->len;
	g_ptr_array_add (mem_types, value);
	g_hash_table_insert (mem_types_ht, value, GUINT_TO_POINTER (mem_id));
	return mem_id;
}

const gchar *
om_mem_type_id_to_string (guint mem_id)
{
	if (mem_types == NULL || mem_id >= mem_types->len)
		return NULL;
	else
		return g_ptr_array_index (mem_types, mem_id);
}
