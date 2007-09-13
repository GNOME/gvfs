#ifndef __G_FILE_ATTRIBUTE_H__
#define __G_FILE_ATTRIBUTE_H__

#include <glib-object.h>
#include <gio/giotypes.h>

G_BEGIN_DECLS

typedef enum {
  G_FILE_ATTRIBUTE_TYPE_INVALID = 0,
  G_FILE_ATTRIBUTE_TYPE_STRING,
  G_FILE_ATTRIBUTE_TYPE_BYTE_STRING,
  G_FILE_ATTRIBUTE_TYPE_UINT32,
  G_FILE_ATTRIBUTE_TYPE_INT32,
  G_FILE_ATTRIBUTE_TYPE_UINT64,
  G_FILE_ATTRIBUTE_TYPE_INT64,
  G_FILE_ATTRIBUTE_TYPE_OBJECT
} GFileAttributeType;

#define G_FILE_ATTRIBUTE_VALUE_INIT {0}

typedef struct  {
  GFileAttributeType type;
  union {
    gint32 int32;
    guint32 uint32;
    gint64 int64;
    guint64 uint64;
    char *string;
    GQuark quark;
    GObject *obj;
  } u;
} GFileAttributeValue;


GFileAttributeValue *g_file_attribute_value_new             (void);
void                 g_file_attribute_value_free            (GFileAttributeValue *attr);
void                 g_file_attribute_value_destroy         (GFileAttributeValue *attr);
void                 g_file_attribute_value_clear           (GFileAttributeValue *attr);
void                 g_file_attribute_value_set             (GFileAttributeValue *attr,
							     GFileAttributeValue *new_value);
GFileAttributeValue *g_file_attribute_value_dup             (GFileAttributeValue *attr);

char *               g_file_attribute_value_as_string       (GFileAttributeValue *attr);

const char *         g_file_attribute_value_get_string      (GFileAttributeValue *attr);
const char *         g_file_attribute_value_get_byte_string (GFileAttributeValue *attr);
guint32              g_file_attribute_value_get_uint32      (GFileAttributeValue *attr);
gint32               g_file_attribute_value_get_int32       (GFileAttributeValue *attr);
guint64              g_file_attribute_value_get_uint64      (GFileAttributeValue *attr);
gint64               g_file_attribute_value_get_int64       (GFileAttributeValue *attr);
GObject *            g_file_attribute_value_get_object      (GFileAttributeValue *attr);

void                 g_file_attribute_value_set_string      (GFileAttributeValue *attr,
							     const char          *value);
void                 g_file_attribute_value_set_byte_string (GFileAttributeValue *attr,
							     const char          *value);
void                 g_file_attribute_value_set_uint32      (GFileAttributeValue *attr,
							     guint32              value);
void                 g_file_attribute_value_set_int32       (GFileAttributeValue *attr,
							     gint32               value);
void                 g_file_attribute_value_set_uint64      (GFileAttributeValue *attr,
							     guint64              value);
void                 g_file_attribute_value_set_int64       (GFileAttributeValue *attr,
							     gint64               value);
void                 g_file_attribute_value_set_object      (GFileAttributeValue *attr,
							     GObject             *obj);

G_END_DECLS


#endif /* __G_FILE_INFO_H__ */
