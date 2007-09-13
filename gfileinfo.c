#include <config.h>

#include <string.h>

#include <gvfstypes.h>
#include "gfileinfo.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GFileInfo, g_file_info, G_TYPE_OBJECT);

static GObjectClass *parent_class = NULL;

typedef struct  {
  GQuark namespace_q;
  GQuark attribute_q; /* full namespace:attribute form */
  char *value;
} GFileAttributeInternal;


struct _GFileInfoPrivate {
  GFileType file_type;
  char *name;
  char *display_name;
  char *edit_name;
  char *icon;
  GQuark mime_type_q;
  goffset size;
  time_t mtime;
  GFileAccessRights access_rights;
  struct stat *stat_info;
  char *symlink_target;
  GArray *attributes;
};

static void
g_file_info_finalize (GObject *object)
{
  GFileInfo *info;
  GFileInfoPrivate *priv;
  GFileAttributeInternal *internal;
  GArray *attrs;
  int i;

  info = G_FILE_INFO (object);

  priv = info->priv;
  
  g_free (priv->name);
  g_free (priv->display_name);
  g_free (priv->icon);
  g_free (priv->stat_info);
  g_free (priv->symlink_target);

  attrs = info->priv->attributes;
  for (i = 0; i < attrs->len; i++)
    {
      internal = &g_array_index (attrs, GFileAttributeInternal, i);
      g_free (internal->value);
    }
  
  g_array_free (priv->attributes, TRUE);  
  
  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
g_file_info_class_init (GFileInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GFileInfoPrivate));
  
  gobject_class->finalize = g_file_info_finalize;
}

static void
g_file_info_init (GFileInfo *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_FILE_INFO,
					    GFileInfoPrivate);

  info->priv->attributes = g_array_new (FALSE,
					FALSE,
					sizeof (GFileAttributeInternal));
}

GFileType
g_file_info_get_file_type (GFileInfo *info)
{
  return info->priv->file_type;
}

const char *
g_file_info_get_name (GFileInfo *info)
{
  return info->priv->name;
}

const char *
g_file_info_get_display_name (GFileInfo *info)
{
  return info->priv->display_name;
}

const char *
g_file_info_get_icon (GFileInfo *info)
{
  return info->priv->display_name;
}

const char *
g_file_info_get_mime_type (GFileInfo *info)
{
  return g_quark_to_string (info->priv->mime_type_q);
}

GQuark
g_file_info_get_mime_type_quark (GFileInfo *info)
{
  return info->priv->mime_type_q;
}

goffset
g_file_info_get_size (GFileInfo         *info)
{
  return info->priv->size;
}
  
time_t
g_file_info_get_modification_time (GFileInfo *info)
{
  return info->priv->mtime;
}

const char *
g_file_info_get_symlink_target (GFileInfo *info)
{
  return info->priv->symlink_target;
}

GFileAccessRights
g_file_get_access_rights (GFileInfo *info)
{
  return info->priv->access_rights;
}

gboolean
g_file_info_can_read (GFileInfo *info)
{
  return info->priv->access_rights & G_FILE_ACCESS_CAN_READ;
}

gboolean
g_file_info_can_write (GFileInfo *info)
{
  return info->priv->access_rights & G_FILE_ACCESS_CAN_WRITE;
}

gboolean
g_file_info_can_delete (GFileInfo *info)
{
  return info->priv->access_rights & G_FILE_ACCESS_CAN_DELETE;
}

gboolean
g_file_info_can_rename (GFileInfo *info)
{
  return info->priv->access_rights & G_FILE_ACCESS_CAN_RENAME;
}
  
const struct stat *
g_file_info_get_stat_info (GFileInfo *info)
{
  return info->priv->stat_info;
}

const char *
g_file_info_get_attribute (GFileInfo *info,
			   const char *attribute)
{
  GFileAttributeInternal *internal;
  GQuark attr_q;
  GArray *attrs;
  int i;
  
  attr_q = g_quark_try_string (attribute);
  if (attr_q == 0)
    return NULL;

  attrs = info->priv->attributes;
  for (i = 0; i < attrs->len; i++)
    {
      internal = &g_array_index (attrs, GFileAttributeInternal, i);
      if (internal->attribute_q == attr_q)
	return internal->value;
    }
  
  return NULL;
}
  
const GFileAttribute *
g_file_info_get_attributes (GFileInfo *info,
			    const char *namespace,
			    int *n_attributes)
{
  GFileAttributeInternal *internal;
  GQuark namespace_q;
  GArray *attrs;
  GArray *result;
  int i;

  *n_attributes = 0;
  
  namespace_q = g_quark_try_string (namespace);
  if (namespace_q == 0)
    return NULL;

  result = g_array_new (FALSE, FALSE, sizeof (GFileAttribute));
  
  attrs = info->priv->attributes;
  for (i = 0; i < attrs->len; i++)
    {
      internal = &g_array_index (attrs, GFileAttributeInternal, i);
      
      if (internal->namespace_q == namespace_q)
	{
	  GFileAttribute attr;
	  attr.attribute = (char *)g_quark_to_string (internal->attribute_q);
	  attr.value = (char *)internal->value;
	  g_array_append_val (result, attr);
	}
    }


  *n_attributes = result->len;

  if (result->len == 0)
    {
      g_array_free (result, TRUE);
      return NULL;
    }

  return (GFileAttribute *)g_array_free (result, FALSE);
}
  
const GFileAttribute *
g_file_info_get_all_attributes (GFileInfo *info,
				int *n_attributes)
{
  GFileAttributeInternal *internal;
  GArray *attrs;
  GArray *result;
  int i;

  *n_attributes = 0;
  
  result = g_array_new (FALSE, FALSE, sizeof (GFileAttribute));
  
  attrs = info->priv->attributes;
  for (i = 0; i < attrs->len; i++)
    {
      GFileAttribute attr;
      internal = &g_array_index (attrs, GFileAttributeInternal, i);
      
      attr.attribute = (char *)g_quark_to_string (internal->attribute_q);
      attr.value = (char *)internal->value;
      g_array_append_val (result, attr);
    }


  *n_attributes = result->len;

  if (result->len == 0)
    {
      g_array_free (result, TRUE);
      return NULL;
    }

  return (GFileAttribute *)g_array_free (result, FALSE);
}  

void
g_file_info_set_file_type (GFileInfo *info,
			   GFileType file_type)
{
  info->priv->file_type = file_type;
}
  
void
g_file_info_set_name (GFileInfo *info,
		      const char *name)
{
  g_free (info->priv->name);
  info->priv->name = g_strdup (name);
}

void
g_file_info_set_display_name (GFileInfo *info,
			      const char *display_name)
{
  g_free (info->priv->display_name);
  info->priv->display_name = g_strdup (display_name);
}

void
g_file_info_set_icon (GFileInfo *info,
		      const char *icon)
{
  g_free (info->priv->icon);
  info->priv->icon = g_strdup (icon);
}

void
g_file_info_set_mime_type (GFileInfo *info,
			   const char *mime_type)
{
  info->priv->mime_type_q = g_quark_from_string (mime_type);
}

void
g_file_info_set_size (GFileInfo *info,
		      goffset size)
{
  info->priv->size = size;
}

void
g_file_info_set_modification_time (GFileInfo *info,
				   time_t mtime)
{
  info->priv->mtime = mtime;
}

void
g_file_info_set_symlink_target (GFileInfo *info,
				const char *link_target)
{
  g_free (info->priv->symlink_target);
  info->priv->symlink_target = g_strdup (link_target);
}
  
void
g_file_set_access_rights (GFileInfo *info,
			  GFileAccessRights access_rights)
{
  info->priv->access_rights = access_rights;
}

void
g_file_info_set_stat_info (GFileInfo *info,
			   const struct stat *statbuf)
{
  if (statbuf == NULL)
    {
      g_free (info->priv->stat_info);
      info->priv->stat_info = NULL;
    }
  else
    {
      if (info->priv->stat_info == NULL)
	info->priv->stat_info = g_new (struct stat, 1);
      *info->priv->stat_info = *statbuf;
    }
}

void
g_file_info_set_attribute (GFileInfo *info,
			   const char *attribute,
			   const char *value)
{
  GFileAttributeInternal new_internal;
  GArray *attrs;
  GQuark attr_q;
  char *colon, *namespace;
  int i;
  
  attr_q = g_quark_from_string (attribute);
  
  attrs = info->priv->attributes;
  for (i = 0; i < attrs->len; i++)
    {
      GFileAttributeInternal *internal = &g_array_index (attrs, GFileAttributeInternal, i);
      
      if (internal->attribute_q == attr_q)
	{
	  g_free (internal->value);
	  internal->value = g_strdup (value);
	  return;
	}
    }

  new_internal.value = g_strdup (value);
  new_internal.attribute_q = g_quark_from_string (attribute);

  colon = strchr (attribute, ':');
  if (colon && colon != attribute) {
    namespace = g_strndup (attribute, colon - attribute);
    new_internal.namespace_q = g_quark_from_string (namespace);
    g_free (namespace);
  } else {
    new_internal.namespace_q = 0;
  }

  g_array_append_val(attrs,new_internal);
}

void
g_file_info_set_attributes (GFileInfo *info,
			    GFileAttribute    *attributes,
			    int                n_attributes)
{
  int i;
  
  for (i = 0; i < n_attributes; i++) {
    g_file_info_set_attribute (info,
			       attributes[i].attribute, 
			       attributes[i].value);
  }
}
