#include <config.h>

#include <string.h>

#include "gfileinfo.h"
#include <glib/gi18n-lib.h>

/* We use this nasty thing, because NULL is a valid attribute matcher (matches nothing) */
#define NO_ATTRIBUTE_MASK ((GFileAttributeMatcher *)1)

typedef struct  {
  guint32 attribute;
  GFileAttributeValue value;
} GFileAttribute;

struct _GFileInfo
{
  GObject parent_instance;

  GArray *attributes;
  GFileAttributeMatcher *mask;
};

static gboolean g_file_attribute_matcher_matches_id (GFileAttributeMatcher *matcher,
						     guint32 id);

G_DEFINE_TYPE (GFileInfo, g_file_info, G_TYPE_OBJECT);

typedef struct {
  guint32 id;
  guint32 attribute_id_counter;
} NSInfo;

G_LOCK_DEFINE_STATIC (attribute_hash);
static int namespace_id_counter = 0;
static GHashTable *ns_hash = NULL;
static GHashTable *attribute_hash = NULL;
static char ***attributes = NULL;

/* Attribute ids are 32bit, we split it up like this:
 * |------------|--------------------|
 *   12 bit          20 bit       
 *   namespace      attribute id    
 *
 * This way the attributes gets sorted in namespace order
 */

#define NS_POS 20
#define NS_MASK ((guint32)((1<<12) - 1))
#define ID_POS 0
#define ID_MASK ((guint32)((1<<20) - 1))

#define GET_NS(_attr_id) \
    (((guint32) (_attr_id) >> NS_POS) & NS_MASK)
#define GET_ID(_attr_id) \
    (((guint32)(_attr_id) >> ID_POS) & ID_MASK)

#define MAKE_ATTR_ID(_ns, _id)				\
    ( ((((guint32) _ns) & NS_MASK) << NS_POS) |		\
      ((((guint32) _id) & ID_MASK) << ID_POS) )

static NSInfo *
_lookup_namespace (const char *namespace)
{
  NSInfo *ns_info;
  
  ns_info = g_hash_table_lookup (ns_hash, namespace);
  if (ns_info == NULL)
    {
      ns_info = g_new0 (NSInfo, 1);
      ns_info->id = ++namespace_id_counter;
      g_hash_table_insert (ns_hash, g_strdup (namespace), ns_info);
      attributes = g_realloc (attributes, (ns_info->id + 1) * sizeof (char **));
      attributes[ns_info->id] = NULL;
    }
  return ns_info;
}

static guint32
lookup_namespace (const char *namespace)
{
  NSInfo *ns_info;
  guint32 id;
  
  G_LOCK (attribute_hash);
  
  if (attribute_hash == NULL)
    {
      ns_hash = g_hash_table_new (g_str_hash, g_str_equal);
      attribute_hash = g_hash_table_new (g_str_hash, g_str_equal);
    }

  ns_info = _lookup_namespace (namespace);
  id = 0;
  if (ns_info)
    id = ns_info->id;
  
  G_UNLOCK (attribute_hash);

  return id;
}

static char *
get_attribute_for_id (int attribute)
{
  char *s;
  G_LOCK (attribute_hash);
  s = attributes[GET_NS(attribute)][GET_ID(attribute)];
  G_UNLOCK (attribute_hash);
  return s;
}

static guint32
lookup_attribute (const char *attribute)
{
  guint32 attr_id, id;
  char *ns;
  const char *colon;
  NSInfo *ns_info;
  
  G_LOCK (attribute_hash);
  if (attribute_hash == NULL)
    {
      ns_hash = g_hash_table_new (g_str_hash, g_str_equal);
      attribute_hash = g_hash_table_new (g_str_hash, g_str_equal);
    }

  attr_id = GPOINTER_TO_UINT (g_hash_table_lookup (attribute_hash, attribute));

  if (attr_id != 0)
    {
      G_UNLOCK (attribute_hash);
      return attr_id;
    }

  colon = strchr (attribute, ':');
  if (colon)
    ns = g_strndup (attribute, colon - attribute);
  else
    ns = g_strdup ("");

  ns_info = _lookup_namespace (ns);
  g_free (ns);

  id = ++ns_info->attribute_id_counter;
  attributes[ns_info->id] = g_realloc (attributes[ns_info->id], (id + 1) * sizeof (char *));
  attributes[ns_info->id][id] = g_strdup (attribute);
  
  attr_id = MAKE_ATTR_ID (ns_info->id, id);

  g_hash_table_insert (attribute_hash, attributes[ns_info->id][id], GUINT_TO_POINTER (attr_id));
  
  G_UNLOCK (attribute_hash);
  
  return attr_id;
}

static void
g_file_info_finalize (GObject *object)
{
  GFileInfo *info;
  int i;
  GFileAttribute *attrs;

  info = G_FILE_INFO (object);

  attrs = (GFileAttribute *)info->attributes->data;
  for (i = 0; i < info->attributes->len; i++)
    g_file_attribute_value_clear (&attrs[i].value);
  g_array_free (info->attributes, TRUE);  

  if (info->mask != NO_ATTRIBUTE_MASK)
    g_file_attribute_matcher_unref (info->mask);
  
  if (G_OBJECT_CLASS (g_file_info_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_info_parent_class)->finalize) (object);
}

static void
g_file_info_class_init (GFileInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_info_finalize;
}

static void
g_file_info_init (GFileInfo *info)
{
  info->mask = NO_ATTRIBUTE_MASK;
  info->attributes = g_array_new (FALSE, FALSE,
				  sizeof (GFileAttribute));
}

GFileInfo *
g_file_info_new (void)
{
  return g_object_new (G_TYPE_FILE_INFO, NULL);
}

void
g_file_info_copy_into (GFileInfo *src_info, GFileInfo *dest_info)
{
  GFileAttribute *source, *dest;
  int i;

  dest = (GFileAttribute *)dest_info->attributes->data;
  for (i = 0; i < dest_info->attributes->len; i++)
    g_file_attribute_value_clear (&dest[i].value);
  
  g_array_set_size (dest_info->attributes,
		    src_info->attributes->len);

  source = (GFileAttribute *)src_info->attributes->data;
  dest = (GFileAttribute *)dest_info->attributes->data;
  
  for (i = 0; i < src_info->attributes->len; i++)
    {
      dest[i].attribute = source[i].attribute;
      dest[i].value.type = G_FILE_ATTRIBUTE_TYPE_INVALID;
      g_file_attribute_value_set (&dest[i].value, &source[i].value);
    }

  if (src_info->mask == NO_ATTRIBUTE_MASK)
    dest_info->mask = NO_ATTRIBUTE_MASK;
  else
    dest_info->mask = g_file_attribute_matcher_ref (src_info->mask);
}

GFileInfo *
g_file_info_dup (GFileInfo  *other)
{
  GFileInfo *new;
  
  new = g_file_info_new ();
  g_file_info_copy_into (other, new);
  return new;
}

void
g_file_info_set_attribute_mask (GFileInfo *info,
				GFileAttributeMatcher *mask)
{
  if (mask != info->mask)
    {
      if (info->mask != NO_ATTRIBUTE_MASK)
	g_file_attribute_matcher_unref (info->mask);
      info->mask = g_file_attribute_matcher_ref (mask);
    }
}

void
g_file_info_unset_attribute_mask (GFileInfo *info)
{
  if (info->mask != NO_ATTRIBUTE_MASK)
    g_file_attribute_matcher_unref (info->mask);
  info->mask = NO_ATTRIBUTE_MASK;
}

void
g_file_info_clear_status (GFileInfo  *info)
{
  GFileAttribute *attrs;
  int i;
  
  attrs = (GFileAttribute *)info->attributes->data;
  for (i = 0; i < info->attributes->len; i++)
    attrs[i].value.status = G_FILE_ATTRIBUTE_STATUS_UNSET;
}

static int
g_file_info_find_place (GFileInfo  *info,
			guint32 attribute)
{
  int min, max, med;
  GFileAttribute *attrs;
  /* Binary search for the place where attribute would be, if its
     in the array */

  min = 0;
  max = info->attributes->len;

  attrs = (GFileAttribute *)info->attributes->data;

  while (min < max)
    {
      med = min + (max - min) / 2;
      if (attrs[med].attribute == attribute)
	{
	  min = med;
	  break;
	}
      else if (attrs[med].attribute < attribute)
	min = med + 1;
      else /* attrs[med].attribute > attribute */
	max = med;
    }

  return min;
}

static GFileAttributeValue *
g_file_info_find_value (GFileInfo *info,
			guint32 attr_id)
{
  GFileAttribute *attrs;
  int i;

  i = g_file_info_find_place (info, attr_id);
  attrs = (GFileAttribute *)info->attributes->data;
  if (i < info->attributes->len &&
      attrs[i].attribute == attr_id)
    return &attrs[i].value;
  
  return NULL;
}

static GFileAttributeValue *
g_file_info_find_value_by_name (GFileInfo *info,
				const char *attribute)
{
  guint32 attr_id;

  attr_id = lookup_attribute (attribute);
  return g_file_info_find_value (info, attr_id);
}


gboolean
g_file_info_has_attribute (GFileInfo  *info,
			   const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return value != NULL;
}

char **
g_file_info_list_attributes (GFileInfo  *info,
			     const char *name_space)
{
  GPtrArray *names;
  GFileAttribute *attrs;
  guint32 attribute;
  int i;

  names = g_ptr_array_new ();
  attrs = (GFileAttribute *)info->attributes->data;
  for (i = 0; i < info->attributes->len; i++)
    {
      attribute = attrs[i].attribute;
      g_ptr_array_add (names, g_strdup (get_attribute_for_id (attribute)));
    }

  /* NULL terminate */
  g_ptr_array_add (names, NULL);
  
  return (char **)g_ptr_array_free (names, FALSE);
}


GFileAttributeType
g_file_info_get_attribute_type (GFileInfo  *info,
				const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  if (value)
    return value->type;
  else
    return G_FILE_ATTRIBUTE_TYPE_INVALID;
}

void
g_file_info_remove_attribute (GFileInfo  *info,
			      const char *attribute)
{
  guint32 attr_id;
  GFileAttribute *attrs;
  int i;

  attr_id = lookup_attribute (attribute);
  
  i = g_file_info_find_place (info, attr_id);
  attrs = (GFileAttribute *)info->attributes->data;
  if (i < info->attributes->len &&
      attrs[i].attribute == attr_id)
    {
      g_file_attribute_value_clear (&attrs[i].value);
      g_array_remove_index (info->attributes, i);
    }
}

const GFileAttributeValue *
g_file_info_get_attribute (GFileInfo  *info,
			   const char *attribute)
  
{
  return g_file_info_find_value_by_name (info, attribute);
}

GObject *
g_file_info_get_attribute_object (GFileInfo  *info,
				  const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_object (value);
}

const char *
g_file_info_get_attribute_string (GFileInfo  *info,
				  const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_string (value);
}

const char *
g_file_info_get_attribute_byte_string (GFileInfo  *info,
				       const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_byte_string (value);
}

gboolean
g_file_info_get_attribute_boolean (GFileInfo  *info,
				   const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_boolean (value);
}

guint32
g_file_info_get_attribute_uint32 (GFileInfo  *info,
				  const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_uint32 (value);
}

gint32
g_file_info_get_attribute_int32 (GFileInfo  *info,
				 const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_int32 (value);
}

guint64
g_file_info_get_attribute_uint64 (GFileInfo  *info,
				  const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_uint64 (value);
}

gint64
g_file_info_get_attribute_int64  (GFileInfo  *info,
				  const char *attribute)
{
  GFileAttributeValue *value;

  value = g_file_info_find_value_by_name (info, attribute);
  return g_file_attribute_value_get_int64 (value);
}

static GFileAttributeValue *
g_file_info_create_value (GFileInfo *info,
			  guint32 attr_id)
{
  GFileAttribute *attrs;
  GFileAttribute attr;
  int i;

  if (info->mask != NO_ATTRIBUTE_MASK &&
      !g_file_attribute_matcher_matches_id (info->mask, attr_id))
    return NULL;
  
  i = g_file_info_find_place (info, attr_id);
  
  attrs = (GFileAttribute *)info->attributes->data;
  if (i < info->attributes->len &&
      attrs[i].attribute == attr_id)
    return &attrs[i].value;
  else
    {
      attr.attribute = attr_id;
      attr.value.type = G_FILE_ATTRIBUTE_TYPE_INVALID;
      g_array_insert_val (info->attributes, i, attr);

      attrs = (GFileAttribute *)info->attributes->data;
      return &attrs[i].value;
    }
}

static GFileAttributeValue *
g_file_info_create_value_by_name (GFileInfo *info,
				  const char *attribute)
{
  guint32 attr_id;

  attr_id = lookup_attribute (attribute);

  return g_file_info_create_value (info, attr_id);
}

void
g_file_info_set_attribute (GFileInfo  *info,
			   const char *attribute,
			   const GFileAttributeValue *attr_value)
{
  GFileAttributeValue *value;
  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set (value, attr_value);
}

void
g_file_info_set_attribute_object (GFileInfo  *info,
				  const char *attribute,
				  GObject *attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_object (value, attr_value);
}

void
g_file_info_set_attribute_string (GFileInfo  *info,
				  const char *attribute,
				  const char *attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_string (value, attr_value);
}

void
g_file_info_set_attribute_byte_string (GFileInfo  *info,
				       const char *attribute,
				       const char *attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_byte_string (value, attr_value);
}

void
g_file_info_set_attribute_boolean (GFileInfo  *info,
				   const char *attribute,
				   gboolean attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_boolean (value, attr_value);
}

void
g_file_info_set_attribute_uint32 (GFileInfo  *info,
				  const char *attribute,
				  guint32     attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_uint32 (value, attr_value);
}

void
g_file_info_set_attribute_int32  (GFileInfo  *info,
				  const char *attribute,
				  gint32      attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_int32 (value, attr_value);
}

void
g_file_info_set_attribute_uint64 (GFileInfo  *info,
				  const char *attribute,
				  guint64     attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_uint64 (value, attr_value);
}

void
g_file_info_set_attribute_int64  (GFileInfo  *info,
				  const char *attribute,
				  gint64      attr_value)
{
  GFileAttributeValue *value;

  value = g_file_info_create_value_by_name (info, attribute);
  if (value)
    g_file_attribute_value_set_int64 (value, attr_value);
}

/* Helper getters */
GFileType
g_file_info_get_file_type (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_TYPE);
  
  value = g_file_info_find_value (info, attr);
  return (GFileType)g_file_attribute_value_get_uint32 (value);
}

gboolean
g_file_info_get_is_hidden (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_IS_HIDDEN);
  
  value = g_file_info_find_value (info, attr);
  return (GFileType)g_file_attribute_value_get_boolean (value);
}

gboolean
g_file_info_get_is_symlink (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_IS_SYMLINK);
  
  value = g_file_info_find_value (info, attr);
  return (GFileType)g_file_attribute_value_get_boolean (value);
}

const char *
g_file_info_get_name (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_NAME);
  
  value = g_file_info_find_value (info, attr);
  return g_file_attribute_value_get_byte_string (value);
}

const char *
g_file_info_get_display_name (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_DISPLAY_NAME);
  
  value = g_file_info_find_value (info, attr);
  return g_file_attribute_value_get_string (value);
}

const char *
g_file_info_get_edit_name (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_EDIT_NAME);
  
  value = g_file_info_find_value (info, attr);
  return g_file_attribute_value_get_string (value);
}

GIcon *
g_file_info_get_icon (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  GObject *obj;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_ICON);
  
  value = g_file_info_find_value (info, attr);
  obj = g_file_attribute_value_get_object (value);
  if (G_IS_ICON (obj))
    return G_ICON (obj);
  return NULL;
}

const char *
g_file_info_get_content_type (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_CONTENT_TYPE);
  
  value = g_file_info_find_value (info, attr);
  return g_file_attribute_value_get_string (value);
}

goffset
g_file_info_get_size (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_SIZE);
  
  value = g_file_info_find_value (info, attr);
  return (goffset) g_file_attribute_value_get_uint64 (value);
}

void
g_file_info_get_modification_time (GFileInfo *info,
				   GTimeVal  *result)
{
  static guint32 attr_mtime = 0, attr_mtime_usec;
  GFileAttributeValue *value;
  
  if (attr_mtime == 0)
    {
      attr_mtime = lookup_attribute (G_FILE_ATTRIBUTE_TIME_MODIFIED);
      attr_mtime_usec = lookup_attribute (G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    }
  
  value = g_file_info_find_value (info, attr_mtime);
  result->tv_sec = g_file_attribute_value_get_uint64 (value);
  value = g_file_info_find_value (info, attr_mtime_usec);
  result->tv_usec = g_file_attribute_value_get_uint32 (value);
}

const char *
g_file_info_get_symlink_target (GFileInfo *info)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET);
  
  value = g_file_info_find_value (info, attr);
  return g_file_attribute_value_get_byte_string (value);
}

/* Helper setters: */

void
g_file_info_set_file_type (GFileInfo         *info,
			   GFileType          type)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_TYPE);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_uint32 (value, type);
}


void
g_file_info_set_is_hidden (GFileInfo *info,
			   gboolean   is_hidden)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_IS_HIDDEN);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_boolean (value, is_hidden);
}

void
g_file_info_set_is_symlink (GFileInfo *info,
			    gboolean   is_symlink)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_IS_SYMLINK);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_boolean (value, is_symlink);
}

void
g_file_info_set_name (GFileInfo         *info,
		      const char        *name)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_NAME);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_byte_string (value, name);
}

void
g_file_info_set_display_name (GFileInfo         *info,
			      const char        *display_name)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_DISPLAY_NAME);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_string (value, display_name);
}

void
g_file_info_set_edit_name (GFileInfo         *info,
			   const char        *edit_name)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_EDIT_NAME);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_string (value, edit_name);
}

void
g_file_info_set_icon (GFileInfo   *info,
		      GIcon       *icon)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_ICON);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_object (value, G_OBJECT (icon));
}

void
g_file_info_set_content_type (GFileInfo         *info,
			      const char        *content_type)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_CONTENT_TYPE);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_string (value, content_type);
}

void
g_file_info_set_size (GFileInfo         *info,
		      goffset            size)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_SIZE);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_uint64 (value, size);
}

void
g_file_info_set_modification_time (GFileInfo         *info,
				   GTimeVal          *mtime)
{
  static guint32 attr_mtime = 0, attr_mtime_usec;
  GFileAttributeValue *value;
  
  if (attr_mtime == 0)
    {
      attr_mtime = lookup_attribute (G_FILE_ATTRIBUTE_TIME_MODIFIED);
      attr_mtime_usec = lookup_attribute (G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    }
  
  value = g_file_info_create_value (info, attr_mtime);
  if (value)
    g_file_attribute_value_set_uint64 (value, mtime->tv_sec);
  value = g_file_info_create_value (info, attr_mtime_usec);
  if (value)
    g_file_attribute_value_set_uint32 (value, mtime->tv_usec);
}

void
g_file_info_set_symlink_target (GFileInfo         *info,
				const char        *symlink_target)
{
  static guint32 attr = 0;
  GFileAttributeValue *value;
  
  if (attr == 0)
    attr = lookup_attribute (G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET);
  
  value = g_file_info_create_value (info, attr);
  if (value)
    g_file_attribute_value_set_byte_string (value, symlink_target);
}

#define ON_STACK_MATCHERS 5

typedef struct {
  guint32 id;
  guint32 mask;
} SubMatcher;

struct _GFileAttributeMatcher {
  gboolean all;
  SubMatcher sub_matchers[ON_STACK_MATCHERS];
  GArray *more_sub_matchers;

  /* Interator */
  guint32 iterator_ns;
  int iterator_pos;
  int ref;
};

static void
matcher_add (GFileAttributeMatcher *matcher,
	     guint id, guint mask)
{
  SubMatcher *sub_matchers;
  int i;
  SubMatcher s;

  for (i = 0; i < ON_STACK_MATCHERS; i++)
    {
      /* First empty spot, not found, use this */
      if (matcher->sub_matchers[i].id == 0)
	{
	  matcher->sub_matchers[i].id = id;
	  matcher->sub_matchers[i].mask = mask;
	  return;
	}
      
      /* Already added */
      if (matcher->sub_matchers[i].id == id &&
	  matcher->sub_matchers[i].mask == mask)
	return;
    }

  if (matcher->more_sub_matchers == NULL)
    matcher->more_sub_matchers = g_array_new (FALSE, FALSE, sizeof (SubMatcher));
      
  sub_matchers = (SubMatcher *)matcher->more_sub_matchers->data;
  for (i = 0; i < matcher->more_sub_matchers->len; i++)
    {
      /* Already added */
      if (sub_matchers[i].id == id &&
	  sub_matchers[i].mask == mask)
	return;
    }

  s.id = id;
  s.mask = mask;
  
  g_array_append_val (matcher->more_sub_matchers, s);
}


GFileAttributeMatcher *
g_file_attribute_matcher_new (const char *attributes)
{
  char **split;
  char *colon;
  int i;
  GFileAttributeMatcher *matcher;

  if (attributes == NULL || *attributes == 0)
    return NULL;

  matcher = g_malloc0 (sizeof (GFileAttributeMatcher));
  matcher->ref = 1;

  split = g_strsplit (attributes, ",", -1);

  for (i = 0; split[i] != NULL; i++)
    {
      if (strcmp (split[i], "*") == 0)
	matcher->all = TRUE;
      else
	{
	  guint32 id, mask;
  
	  colon = strchr (split[i], ':');
	  if (colon != NULL &&
	      !(colon[1] == 0 ||
		(colon[1] == '*' &&
		 colon[2] == 0)))
	    {
	      id = lookup_attribute (split[i]);
	      mask = 0xffffffff;
	    }
	  else
	    {
	      if (colon)
		*colon = 0;

	      id = lookup_namespace (split[i]) << NS_POS;
	      mask = NS_MASK << NS_POS;
	    }
	  
	  matcher_add (matcher, id, mask);
	}
    }

  g_strfreev (split);

  return matcher;
}

GFileAttributeMatcher *
g_file_attribute_matcher_ref (GFileAttributeMatcher *matcher)
{
  if (matcher)
    matcher->ref++;
  
  return matcher;
}

void
g_file_attribute_matcher_unref (GFileAttributeMatcher *matcher)
{
  if (matcher == NULL)
    return;

  matcher->ref--;

  if (matcher->ref == 0)
    {
      if (matcher->more_sub_matchers)
	g_array_free (matcher->more_sub_matchers, TRUE);
      
      g_free (matcher);
    }
}

gboolean
g_file_attribute_matcher_matches_only (GFileAttributeMatcher *matcher,
				       const char            *attribute)
{
  guint32 id;

  if (matcher == NULL)
    return FALSE;
  
  if (matcher->all)
    return FALSE;
  
  id = lookup_attribute (attribute);

  if (matcher->sub_matchers[0].id != 0 &&
      matcher->sub_matchers[1].id == 0 &&
      matcher->sub_matchers[0].id == (id & matcher->sub_matchers[0].mask))
    return TRUE;
  
  return FALSE;
}

static gboolean
matcher_matches_id (GFileAttributeMatcher *matcher,
		    guint32 id)
{
  SubMatcher *sub_matchers;
  int i;
  
  for (i = 0; i < ON_STACK_MATCHERS; i++)
    {
      if (matcher->sub_matchers[i].id == 0)
	return FALSE;
      
      if (matcher->sub_matchers[i].id == (id & matcher->sub_matchers[i].mask))
	return TRUE;
    }

  if (matcher->more_sub_matchers)
    {
      sub_matchers = (SubMatcher *)matcher->more_sub_matchers->data;
      for (i = 0; i < matcher->more_sub_matchers->len; i++)
	{
	  if (matcher->sub_matchers[i].id == (id & matcher->sub_matchers[i].mask))
	    return TRUE;
	}
    }
  
  return FALSE;
}

static gboolean
g_file_attribute_matcher_matches_id (GFileAttributeMatcher *matcher,
				     guint32 id)
{
  if (matcher == NULL)
    return FALSE;
  
  if (matcher->all)
    return TRUE;
  
  return matcher_matches_id (matcher, id);
}

gboolean
g_file_attribute_matcher_matches (GFileAttributeMatcher *matcher,
				  const char            *attribute)
{
  if (matcher == NULL)
    return FALSE;
  
  if (matcher->all)
    return TRUE;
  
  return matcher_matches_id (matcher, lookup_attribute (attribute));
}

/* return TRUE -> all */
gboolean
g_file_attribute_matcher_enumerate_namespace (GFileAttributeMatcher *matcher,
					      const char            *namespace)
{
  SubMatcher *sub_matchers;
  int ns_id;
  int i;
  
  if (matcher == NULL)
    return FALSE;

  if (matcher->all)
    return TRUE;

  ns_id = lookup_namespace (namespace) << NS_POS;

  for (i = 0; i < ON_STACK_MATCHERS; i++)
    {
      if (matcher->sub_matchers[i].id == ns_id)
	return TRUE;
    }

  if (matcher->more_sub_matchers)
    {
      sub_matchers = (SubMatcher *)matcher->more_sub_matchers->data;
      for (i = 0; i < matcher->more_sub_matchers->len; i++)
	{
	  if (matcher->sub_matchers[i].id == ns_id)
	    return TRUE;
	}
    }

  matcher->iterator_ns = ns_id;
  matcher->iterator_pos = 0;
  
  return FALSE;
}

const const char *
g_file_attribute_matcher_enumerate_next (GFileAttributeMatcher *matcher)
{
  int i;
  SubMatcher *sub_matcher;
  
  if (matcher == NULL)
    return NULL;

  while (1)
    {
      i = matcher->iterator_pos++;

      if (i < ON_STACK_MATCHERS)
	{
	  if (matcher->sub_matchers[i].id == 0)
	    return NULL;

	  sub_matcher = &matcher->sub_matchers[i];
	}
      else
	{
	  if (matcher->more_sub_matchers == NULL)
	    return NULL;
      
	  i -= ON_STACK_MATCHERS;
	  if (i < matcher->more_sub_matchers->len)
	    sub_matcher = &g_array_index (matcher->more_sub_matchers, SubMatcher, i);
	  else
	    return NULL;
	}

      if (sub_matcher->mask == 0xffffffff &&
	  (sub_matcher->id & (NS_MASK << NS_POS)) == matcher->iterator_ns)
	return get_attribute_for_id (sub_matcher->id);
    }
}
