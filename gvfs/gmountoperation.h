#ifndef __G_MOUNT_OPERATION_H__
#define __G_MOUNT_OPERATION_H__

#include <sys/stat.h>

#include <glib-object.h>
#include <gvfs/gvfstypes.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_OPERATION         (g_mount_operation_get_type ())
#define G_MOUNT_OPERATION(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_OPERATION, GMountOperation))
#define G_MOUNT_OPERATION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_OPERATION, GMountOperationClass))
#define G_IS_MOUNT_OPERATION(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_OPERATION))
#define G_IS_MOUNT_OPERATION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_OPERATION))
#define G_MOUNT_OPERATION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_OPERATION, GMountOperationClass))

typedef struct _GMountOperation        GMountOperation;
typedef struct _GMountOperationClass   GMountOperationClass;
typedef struct _GMountOperationPrivate GMountOperationPrivate;

struct _GMountOperation
{
  GObject parent_instance;

  GMountOperationPrivate *priv;
};

typedef enum {
  G_PASSWORD_FLAGS_NEED_PASSWORD    = 1<<0,
  G_PASSWORD_FLAGS_NEED_USERNAME    = 1<<1,
  G_PASSWORD_FLAGS_NEED_DOMAIN      = 1<<2,
  G_PASSWORD_FLAGS_SAVING_SUPPORTED = 1<<4,
  G_PASSWORD_FLAGS_ANON_SUPPORTED   = 1<<5
} GPasswordFlags;

typedef enum {
  G_PASSWORD_SAVE_NEVER,
  G_PASSWORD_SAVE_FOR_SESSION,
  G_PASSWORD_SAVE_PERMANENTLY
} GPasswordSave;

struct _GMountOperationClass
{
  GObjectClass parent_class;

  /* signals: */

  gboolean (* ask_password) (GMountOperation *op,
			     const char      *message,
			     const char      *default_user,
			     const char      *default_domain,
			     GPasswordFlags   flags);
  gboolean (* ask_question) (GMountOperation *op,
			     const char      *message,
			     const char      *choices[]);
  
  void     (* done)         (GMountOperation *op,
			     gboolean         succeeded,
			     GError          *error);

  void     (* reply)        (GMountOperation *op,
			     gboolean         abort);
  
  
};

GType g_mount_operation_get_type (void) G_GNUC_CONST;
  
GMountOperation *  g_mount_operation_new (void);

const char *  g_mount_operation_get_username      (GMountOperation *op);
void          g_mount_operation_set_username      (GMountOperation *op,
						   const char      *username);
const char *  g_mount_operation_get_password      (GMountOperation *op);
void          g_mount_operation_set_password      (GMountOperation *op,
						   const char      *password);
gboolean      g_mount_operation_get_anonymous     (GMountOperation *op);
void          g_mount_operation_set_anonymous     (GMountOperation *op,
						   gboolean         anonymous);
const char *  g_mount_operation_get_domain        (GMountOperation *op);
void          g_mount_operation_set_domain        (GMountOperation *op,
						   const char      *domain);
GPasswordSave g_mount_operation_get_password_save (GMountOperation *op);
void          g_mount_operation_set_password_save (GMountOperation *op,
						   GPasswordSave    save);
int           g_mount_operation_get_choice        (GMountOperation *op);
void          g_mount_operation_set_choice        (GMountOperation *op,
						   int              choice);
void          g_mount_operation_reply             (GMountOperation *op,
						   gboolean         abort);

G_END_DECLS

#endif /* __G_MOUNT_OPERATION_H__ */
