#include <config.h>

#include <string.h>

#include <gvfstypes.h>
#include "gfileinfo.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GFileInfo, g_file_info, G_TYPE_OBJECT);

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
  
  if (G_OBJECT_CLASS (g_file_info_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_info_parent_class)->finalize) (object);
}

static void
g_file_info_class_init (GFileInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
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

GFileInfo *
g_file_info_new (void)
{
  return g_object_new (G_TYPE_FILE_INFO, NULL);
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
g_file_info_set_access_rights (GFileInfo *info,
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
g_file_info_set_from_stat (GFileInfo         *info,
			   GFileInfoRequestFlags requested,
			   const struct stat *statbuf)
{
  if (requested && G_FILE_INFO_FILE_TYPE)
    {
      if (S_ISREG (statbuf->st_mode))
	info->priv->file_type = G_FILE_TYPE_REGULAR;
      else if (S_ISDIR (statbuf->st_mode))
	info->priv->file_type = G_FILE_TYPE_DIRECTORY;
      else if (S_ISCHR (statbuf->st_mode) ||
	       S_ISBLK (statbuf->st_mode) ||
	       S_ISFIFO (statbuf->st_mode)
#ifdef S_ISSOCK
	       || S_ISSOCK (statbuf->st_mode)
#endif
	       )
	info->priv->file_type = G_FILE_TYPE_SPECIAL;
      else if (S_ISLNK (statbuf->st_mode))
	info->priv->file_type = G_FILE_TYPE_SYMBOLIC_LINK;
      else 
	info->priv->file_type = G_FILE_TYPE_UNKNOWN;
    }
  
  if (requested && G_FILE_INFO_SIZE)
    g_file_info_set_size (info, statbuf->st_size);

  if (requested && G_FILE_INFO_MODIFICATION_TIME)
    g_file_info_set_modification_time (info, statbuf->st_mtime);
  
  if (requested && G_FILE_INFO_STAT_INFO)
    g_file_info_set_stat_info (info, statbuf);
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

#define ON_STACK_NAMESPACES 3
#define ON_STACK_ATTRIBUTES 3


typedef struct {
  GQuark namespace;
  gboolean all;
  GQuark full_names[ON_STACK_ATTRIBUTES];
  GArray *more_full_names;
} NamespaceMatcher;

struct _GFileAttributeMatcher {
  gboolean all;
  NamespaceMatcher namespaces[ON_STACK_NAMESPACES];
  GArray *more_namespaces;

  /* Interator */
  NamespaceMatcher *matched_namespace;
  int attribute_pos;
};

static NamespaceMatcher *
matcher_find_namespace (GFileAttributeMatcher *matcher,
			GQuark namespace_q,
			gboolean create)
{
  NamespaceMatcher *ns_matcher;
  int i;
  
  for (i = 0; i < ON_STACK_NAMESPACES; i++)
    {
      ns_matcher = &matcher->namespaces[i];

      /* First empty spot, not found, use this */
      if (ns_matcher->namespace == 0)
	{
	  if (create)
	    {
	      ns_matcher->namespace = namespace_q;
	      return ns_matcher;
	    }
	  else
	    return NULL;
	}

      /* Found, use this */
      if (ns_matcher->namespace == namespace_q)
	return ns_matcher;
    }

  if (matcher->more_namespaces == NULL)
    {
      if (create)
	matcher->more_namespaces = g_array_new (FALSE, FALSE, sizeof (NamespaceMatcher));
      else
	return NULL;
    }

  for (i = 0; i < matcher->more_namespaces->len; i++)
    {
      ns_matcher = &g_array_index (matcher->more_namespaces, NamespaceMatcher, i);
      if (ns_matcher->namespace == namespace_q)
	return ns_matcher;
    }
  
  if (create)
    {
      NamespaceMatcher new_space = {namespace_q};
      g_array_append_val (matcher->more_namespaces, new_space);
      ns_matcher = &g_array_index (matcher->more_namespaces, NamespaceMatcher, i);
      return ns_matcher;
    }
  
  return NULL;
}

static void
matcher_add_namespace (GFileAttributeMatcher *matcher,
		       GQuark namespace_q,
		       GQuark full_name_q)
{
  int i;
  NamespaceMatcher *ns_matcher;

  ns_matcher = matcher_find_namespace (matcher, namespace_q, TRUE);

  if (full_name_q == 0)
    ns_matcher->all = TRUE;
  else
    {
      for (i = 0; i < ON_STACK_ATTRIBUTES; i++)
	{
	  
	  /* First empty spot, not found, use this */
	  if (ns_matcher->full_names[i] == 0)
	    {
	      ns_matcher->full_names[i] = full_name_q;
	      break;
	    }

	  /* Already added */
	  if (ns_matcher->full_names[i] == namespace_q)
	    break;
	}
      
      if (i == ON_STACK_ATTRIBUTES)
	{
	  if (ns_matcher->more_full_names == NULL)
	    ns_matcher->more_full_names = g_array_new (FALSE, FALSE, sizeof (GQuark));

	  for (i = 0; i < ns_matcher->more_full_names->len; i++)
	    {
	      GQuark existing_name = g_array_index (ns_matcher->more_full_names, GQuark, i);
	      if (existing_name == full_name_q)
		break;
	    }
	  
	  g_array_append_val (ns_matcher->more_full_names, full_name_q);
	}
    }
}


GFileAttributeMatcher *
g_file_attribute_matcher_new (const char *attributes)
{
  char **split;
  char *colon;
  int i;
  int num_ns, num_fn;
  GQuark full_name_q, namespace_q;
  GArray *full_name_array, *namespace_array;
  GFileAttributeMatcher *matcher;

  if (attributes == NULL)
    return NULL;

  matcher = g_malloc0 (sizeof (GFileAttributeMatcher));

  split = g_strsplit (attributes, ",", -1);

  num_ns = 0;
  num_fn = 0;

  full_name_array = NULL;
  namespace_array = NULL;
  
  for (i = 0; split[i] != NULL; i++)
    {
      if (strcmp (split[i], "*") == 0)
	matcher->all = TRUE;
      else
	{
	  colon = strchr (split[i], ':');

	  full_name_q = 0;
	  if (colon != NULL && colon[1] != 0)
	    {
	      full_name_q = g_quark_from_string (split[i]);
	      *colon = 0;
	    }
	  
	  namespace_q = g_quark_from_string (split[i]);
	  matcher_add_namespace (matcher, namespace_q, full_name_q);
	}
    }

  g_strfreev (split);

  return matcher;
}

void
g_file_attribute_matcher_free (GFileAttributeMatcher *matcher)
{
  NamespaceMatcher *ns_matcher;
  int i;
  
  if (matcher == NULL)
    return;

  for (i = 0; i < ON_STACK_NAMESPACES; i++)
    {
      ns_matcher = &matcher->namespaces[i];

      if (ns_matcher->more_full_names != NULL)
	g_array_free (ns_matcher->more_full_names, TRUE);
    }
  
  if (matcher->more_namespaces)
    {
      for (i = 0; i < matcher->more_namespaces->len; i++)
	{
	  ns_matcher = &g_array_index (matcher->more_namespaces, NamespaceMatcher, i);

	  if (ns_matcher->more_full_names != NULL)
	    g_array_free (ns_matcher->more_full_names, TRUE);
	}
      
      g_array_free (matcher->more_namespaces, TRUE);
    }
  
  g_free (matcher);
}

gboolean
g_file_attribute_matcher_matches (GFileAttributeMatcher *matcher,
				  const char            *namespace,
				  const char            *full_name)
{
  return g_file_attribute_matcher_matches_q (matcher,
					     g_quark_from_string (namespace),
					     g_quark_from_string (full_name));
}

gboolean
g_file_attribute_matcher_matches_q (GFileAttributeMatcher *matcher,
				    GQuark                 namespace,
				    GQuark                 full_name)
{
  NamespaceMatcher *ns_matcher;
  int i;

  if (matcher == NULL)
    return FALSE;
  
  if (matcher->all)
    return TRUE;

  ns_matcher = matcher_find_namespace (matcher, namespace, FALSE);
  
  if (ns_matcher == NULL)
    return FALSE;
  
  if (ns_matcher->all)
    return TRUE;

  for (i = 0; i < ON_STACK_ATTRIBUTES; i++)
    {
      if (ns_matcher->full_names[i] == 0)
	return FALSE;
      
      if (ns_matcher->full_names[i] == full_name)
	return TRUE;
    }

  if (ns_matcher->more_full_names)
    {
      for (i = 0; i < ns_matcher->more_full_names->len; i++)
	{
	  GQuark existing_name = g_array_index (ns_matcher->more_full_names, GQuark, i);
	  if (existing_name == full_name)
	    return TRUE;
	}
    }
  
  return FALSE;
}


gboolean
g_file_attribute_matcher_enumerate (GFileAttributeMatcher *matcher,
				    const char            *namespace)
{
  return g_file_attribute_matcher_enumerate_q (matcher,
					       g_quark_from_string (namespace));
}

/* return TRUE -> all */
gboolean
g_file_attribute_matcher_enumerate_q (GFileAttributeMatcher *matcher,
				      GQuark                 namespace)
{
  NamespaceMatcher *ns_matcher;

  if (matcher == NULL)
    return FALSE;

  if (matcher->all)
    return TRUE;

  ns_matcher = matcher_find_namespace (matcher, namespace, FALSE);

  matcher->matched_namespace = ns_matcher;
  matcher->attribute_pos = 0;
  
  if (ns_matcher == NULL)
    return FALSE;
  
  if (ns_matcher->all)
    return TRUE;

  return FALSE;
}

const char *
g_file_attribute_matcher_enumerate_next (GFileAttributeMatcher *matcher)
{
  NamespaceMatcher *ns_matcher;
  int i;
  GQuark full_name_q;
  const char *full_name;
  
  if (matcher == NULL)
    return NULL;

  ns_matcher = matcher->matched_namespace;

  if (ns_matcher == NULL)
    return NULL;

  i = matcher->attribute_pos++;

  if (i < ON_STACK_ATTRIBUTES)
    {
      full_name_q = ns_matcher->full_names[i];
      if (full_name_q == 0)
	return NULL;
    }
  else
    {
      if (ns_matcher->more_full_names == NULL)
	return NULL;
      
      i -= ON_STACK_ATTRIBUTES;
      if (i < ns_matcher->more_full_names->len)
	full_name_q = g_array_index (ns_matcher->more_full_names, GQuark, i);
      else
	return NULL;
    }
  
  full_name = g_quark_to_string (full_name_q);

  /* Full names are guaranteed to have a ':' in them */
  return strchr (full_name, ':') + 1;
}
