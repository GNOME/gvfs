#include <config.h>

#include <string.h>

#include "gfileattribute.h"
#include <glib/gi18n-lib.h>

void
g_file_attribute_value_free (GFileAttributeValue *attr)
{
  g_file_attribute_value_clear (attr);
  g_free (attr);
}

void
g_file_attribute_value_clear (GFileAttributeValue *attr)
{
  if (attr->type == G_FILE_ATTRIBUTE_TYPE_STRING ||
      attr->type == G_FILE_ATTRIBUTE_TYPE_BYTE_STRING)
    g_free (attr->u.string);
  
  if (attr->type == G_FILE_ATTRIBUTE_TYPE_OBJECT &&
      attr->u.obj != NULL)
    g_object_unref (attr->u.obj);
  
  attr->type = G_FILE_ATTRIBUTE_TYPE_INVALID;
}

void
g_file_attribute_value_set (GFileAttributeValue *attr,
			    const GFileAttributeValue *new_value)
{
  g_file_attribute_value_clear (attr);
  *attr = *new_value;

  if (attr->type == G_FILE_ATTRIBUTE_TYPE_STRING ||
      attr->type == G_FILE_ATTRIBUTE_TYPE_BYTE_STRING)
    attr->u.string = g_strdup (attr->u.string);
  
  if (attr->type == G_FILE_ATTRIBUTE_TYPE_OBJECT &&
      attr->u.obj != NULL)
    g_object_ref (attr->u.obj);
}

GFileAttributeValue *
g_file_attribute_value_new (void)
{
  GFileAttributeValue *attr;

  attr = g_new (GFileAttributeValue, 1);
  attr->type = G_FILE_ATTRIBUTE_TYPE_INVALID;
  return attr;
}

GFileAttributeValue *
g_file_attribute_value_dup (const GFileAttributeValue *old_attr)
{
  GFileAttributeValue *attr;

  attr = g_new (GFileAttributeValue, 1);
  attr->type = G_FILE_ATTRIBUTE_TYPE_INVALID;
  g_file_attribute_value_set (attr, old_attr);
  return attr;
}

static gboolean
valid_char (char c)
{
  return c >= 32 && c <= 126 && c != '\\';
}

static char *
escape_byte_string (const char *str)
{
  size_t len;
  int num_invalid, i;
  char *escaped_val, *p;
  unsigned char c;
  char *hex_digits = "0123456789abcdef";
  
  len = strlen (str);
  
  num_invalid = 0;
  for (i = 0; i < len; i++)
    {
      if (!valid_char (str[i]))
	num_invalid++;
    }
	
  if (num_invalid == 0)
    return g_strdup (str);
  else
    {
      escaped_val = g_malloc (len + num_invalid*3 + 1);

      p = escaped_val;
      for (i = 0; i < len; i++)
	{
	  c = str[i];
	  if (valid_char (c))
	    *p++ = c;
	  else
	    {
	      *p++ = '\\';
	      *p++ = 'x';
	      *p++ = hex_digits[(c >> 8) & 0xf];
	      *p++ = hex_digits[c & 0xf];
	    }
	}
      *p++ = 0;
      return escaped_val;
    }
}

char *
g_file_attribute_value_as_string (const GFileAttributeValue *attr)
{
  char *str;

  switch (attr->type)
    {
    case G_FILE_ATTRIBUTE_TYPE_STRING:
      str = g_strdup (attr->u.string);
      break;
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      str = escape_byte_string (attr->u.string);
      break;
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      str = g_strdup_printf ("%s", attr->u.boolean?"TRUE":"FALSE");
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      str = g_strdup_printf ("%u", (unsigned int)attr->u.uint32);
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      str = g_strdup_printf ("%i", (int)attr->u.int32);
      break;
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      str = g_strdup_printf ("%"G_GUINT64_FORMAT, attr->u.uint64);
      break;
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      str = g_strdup_printf ("%"G_GINT64_FORMAT, attr->u.int64);
      break;
    default:
      g_warning ("Invalid type in GFileInfo attribute");
      str = g_strdup ("<invalid>");
      break;
    }
  
  return str;
}

const char *
g_file_attribute_value_get_string (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_STRING)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return NULL;
    }
  return attr->u.string;
}

const char *
g_file_attribute_value_get_byte_string (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_BYTE_STRING)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return NULL;
    }
  return attr->u.string;
}
  
gboolean
g_file_attribute_value_get_boolean (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_BOOLEAN)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return FALSE;
    }
  return attr->u.boolean;
}
  
guint32
g_file_attribute_value_get_uint32 (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_UINT32)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return 0;
    }
  return attr->u.uint32;
}
  
gint32
g_file_attribute_value_get_int32 (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_INT32)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return 0;
    }
  return attr->u.int32;
}
  
guint64
g_file_attribute_value_get_uint64 (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_UINT64)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return 0;
    }
  return attr->u.uint64;
}
  
gint64
g_file_attribute_value_get_int64 (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_INT64)
    {
      if (attr != NULL)
	g_warning ("Invalid type in GFileInfo attribute");
      return 0;
    }
  return attr->u.int64;
}

GObject *
g_file_attribute_value_get_object (const GFileAttributeValue *attr)
{
  if (attr == NULL || attr->type != G_FILE_ATTRIBUTE_TYPE_OBJECT)
    {
      if (attr != NULL)
       g_warning ("Invalid type in GFileInfo attribute");
      return NULL;
    }
  if (attr->u.obj)
    return g_object_ref (attr->u.obj);
  return NULL;
}
  
void
g_file_attribute_value_set_string (GFileAttributeValue *attr,
				   const char          *string)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_STRING;
  attr->u.string = g_strdup (string);
}

  
void
g_file_attribute_value_set_byte_string (GFileAttributeValue *attr,
					const char          *string)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_BYTE_STRING;
  attr->u.string = g_strdup (string);
}
  
void
g_file_attribute_value_set_boolean (GFileAttributeValue *attr,
				    gboolean             value)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_BOOLEAN;
  attr->u.boolean = !!value;
}
  
void
g_file_attribute_value_set_uint32 (GFileAttributeValue *attr,
				   guint32              value)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_UINT32;
  attr->u.uint32 = value;
}
  
void
g_file_attribute_value_set_int32 (GFileAttributeValue *attr,
				  gint32               value)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_INT32;
  attr->u.int32 = value;
}

void
g_file_attribute_value_set_uint64 (GFileAttributeValue *attr,
				   guint64              value)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_UINT64;
  attr->u.uint64 = value;
}

void
g_file_attribute_value_set_int64 (GFileAttributeValue *attr,
				  gint64               value)
{
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_INT64;
  attr->u.int64 = value;
}
  
void
g_file_attribute_value_set_object (GFileAttributeValue *attr,
				   GObject             *obj)
  {
  g_file_attribute_value_clear (attr);
  attr->type = G_FILE_ATTRIBUTE_TYPE_OBJECT;
  if (obj)
    attr->u.obj = g_object_ref (obj);
  else
    attr->u.obj = NULL;
}

typedef struct {
  GFileAttributeInfoList public;
  GArray *array;
} GFileAttributeInfoListPriv;

static void
list_update_public (GFileAttributeInfoListPriv *priv)
{
  priv->public.infos = (GFileAttributeInfo *)priv->array->data;
  priv->public.n_infos = priv->array->len;
}

GFileAttributeInfoList *
g_file_attribute_info_list_new (void)
{
  GFileAttributeInfoListPriv *priv;

  priv = g_new0 (GFileAttributeInfoListPriv, 1);
  
  priv->array = g_array_new (TRUE, FALSE, sizeof (GFileAttributeInfo));
  
  list_update_public (priv);
  
  return (GFileAttributeInfoList *)priv;
}

GFileAttributeInfoList *
g_file_attribute_info_list_dup (GFileAttributeInfoList *list)
{
  GFileAttributeInfoListPriv *new;
  int i;

  new = g_new0 (GFileAttributeInfoListPriv, 1);
  new->array = g_array_new (TRUE, FALSE, sizeof (GFileAttributeInfo));

  g_array_set_size (new->array, list->n_infos);
  list_update_public (new);
  for (i = 0; i < list->n_infos; i++)
    {
      new->public.infos[i].name = g_strdup (list->infos[i].name);
      new->public.infos[i].type = list->infos[i].type;
      new->public.infos[i].flags = list->infos[i].flags;
    }
  
  return (GFileAttributeInfoList *)new;
}

void
g_file_attribute_info_list_free (GFileAttributeInfoList *list)
{
  GFileAttributeInfoListPriv *priv = (GFileAttributeInfoListPriv *)list;
  int i;
  for (i = 0; i < list->n_infos; i++)
    g_free (list->infos[i].name);
  g_array_free (priv->array, TRUE);
}

static int
g_file_attribute_info_list_bsearch (GFileAttributeInfoList *list,
				    const char             *name)
{
  int start, end, mid;
  
  start = 0;
  end = list->n_infos;

  while (start != end)
    {
      mid = start + (end - start) / 2;

      if (strcmp (name, list->infos[mid].name) < 0)
	end = mid;
      else if (strcmp (name, list->infos[mid].name) > 0)
	start = mid + 1;
      else
	return mid;
    }
  return start;
}

const GFileAttributeInfo *
g_file_attribute_info_list_lookup (GFileAttributeInfoList *list,
				   const char             *name)
{
  int i;

  if (list == NULL)
    return NULL;
  
  i = g_file_attribute_info_list_bsearch (list, name);

  if (i < list->n_infos && strcmp (list->infos[i].name, name) == 0)
    return &list->infos[i];
  
  return NULL;
}

void
g_file_attribute_info_list_add    (GFileAttributeInfoList *list,
				   const char             *name,
				   GFileAttributeType      type,
				   GFileAttributeFlags     flags)
{
  GFileAttributeInfoListPriv *priv = (GFileAttributeInfoListPriv *)list;
  GFileAttributeInfo info;
  int i;

  i = g_file_attribute_info_list_bsearch (list, name);

  if (i < list->n_infos && strcmp (list->infos[i].name, name) == 0)
    {
      list->infos[i].type = type;
      return;
    }

  info.name = g_strdup (name);
  info.type = type;
  info.flags = flags;
  g_array_insert_vals (priv->array, i, &info, 1);

  list_update_public (priv);
}
