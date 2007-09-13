#include <config.h>

#include <string.h>

#include <gvfstypes.h>
#include "gmountoperation.h"
#include "gvfs-marshal.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GMountOperation, g_mount_operation, G_TYPE_OBJECT);

enum {
  ASK_PASSWORD,
  ASK_QUESTION,
  DONE,
  REPLY,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _GMountOperationPrivate {
  char *password;
  char *user;
  char *domain;
  gboolean anonymous;
  GPasswordSave password_save;
  int choice;
};

static void
g_mount_operation_finalize (GObject *object)
{
  GMountOperation *operation;
  GMountOperationPrivate *priv;

  operation = G_MOUNT_OPERATION (object);

  priv = operation->priv;
  
  g_free (priv->password);
  g_free (priv->user);
  g_free (priv->domain);
  
  if (G_OBJECT_CLASS (g_mount_operation_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_operation_parent_class)->finalize) (object);
}

static gboolean
boolean_handled_accumulator (GSignalInvocationHint *ihint,
			     GValue                *return_accu,
			     const GValue          *handler_return,
			     gpointer               dummy)
{
  gboolean continue_emission;
  gboolean signal_handled;
  
  signal_handled = g_value_get_boolean (handler_return);
  g_value_set_boolean (return_accu, signal_handled);
  continue_emission = !signal_handled;
  
  return continue_emission;
}

static gboolean
ask_password (GMountOperation *op,
	      const char      *message,
	      const char      *default_user,
	      const char      *default_domain,
	      GPasswordFlags   flags)
{
  return FALSE;
}
  
static gboolean
ask_question (GMountOperation *op,
	      const char      *message,
	      const char      *choices[])
{
  return FALSE;
}

static void
g_mount_operation_class_init (GMountOperationClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GMountOperationPrivate));
  
  gobject_class->finalize = g_mount_operation_finalize;
  
  klass->ask_password = ask_password;
  klass->ask_question = ask_question;
  
  signals[ASK_PASSWORD] =
    g_signal_new (I_("ask_password"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GMountOperationClass, ask_password),
		  boolean_handled_accumulator, NULL,
		  _gvfs_marshal_BOOLEAN__STRING_STRING_STRING_INT,
		  G_TYPE_BOOLEAN, 4,
		  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
  
  signals[ASK_QUESTION] =
    g_signal_new (I_("ask_question"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GMountOperationClass, ask_question),
		  boolean_handled_accumulator, NULL,
		  _gvfs_marshal_BOOLEAN__STRING_POINTER,
		  G_TYPE_BOOLEAN, 2,
		  G_TYPE_STRING, G_TYPE_POINTER);

  signals[REPLY] =
    g_signal_new (I_("reply"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GMountOperationClass, reply),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__BOOLEAN,
		  G_TYPE_NONE, 1,
		  G_TYPE_BOOLEAN);

  signals[DONE] =
    g_signal_new (I_("done"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GMountOperationClass, done),
		  NULL, NULL,
		  _gvfs_marshal_VOID__BOOLEAN_POINTER,
		  G_TYPE_NONE, 2,
		  G_TYPE_BOOLEAN,
		  G_TYPE_POINTER);
}

static void
g_mount_operation_init (GMountOperation *operation)
{
  operation->priv = G_TYPE_INSTANCE_GET_PRIVATE (operation,
						 G_TYPE_MOUNT_OPERATION,
						 GMountOperationPrivate);
}

GMountOperation *
g_mount_operation_new (void)
{
  return g_object_new (G_TYPE_MOUNT_OPERATION, NULL);
}

const char *
g_mount_operation_get_username (GMountOperation *op)
{
  return op->priv->user;
}

void
g_mount_operation_set_username (GMountOperation *op,
				const char      *username)
{
  g_free (op->priv->user);
  op->priv->user = g_strdup (username);
}

const char *
g_mount_operation_get_password (GMountOperation *op)
{
  return op->priv->password;
}
  
void
g_mount_operation_set_password (GMountOperation *op,
				const char      *password)
{
  g_free (op->priv->password);
  op->priv->password = g_strdup (password);
}
  
gboolean
g_mount_operation_get_anonymous (GMountOperation *op)
{
  return op->priv->anonymous;
}
  
void
g_mount_operation_set_anonymous (GMountOperation *op,
				 gboolean         anonymous)
{
  op->priv->anonymous = anonymous;
}

const char *
g_mount_operation_get_domain (GMountOperation *op)
{
  return op->priv->password;
}

void
g_mount_operation_set_domain (GMountOperation *op,
			      const char      *domain)
{
  g_free (op->priv->domain);
  op->priv->domain = g_strdup (domain);
}

GPasswordSave
g_mount_operation_get_password_save (GMountOperation *op)
{
  return op->priv->password_save;
}
    
void
g_mount_operation_set_password_save (GMountOperation *op,
				     GPasswordSave    save)
{
  op->priv->password_save = save;
}

int
g_mount_operation_get_choice (GMountOperation *op)
{
  return op->priv->choice;
}

void
g_mount_operation_set_choice (GMountOperation *op,
			      int choice)
{
  op->priv->choice = choice;
}

void
g_mount_operation_reply (GMountOperation *op,
			 gboolean         abort)
{
  g_signal_emit (op, signals[REPLY], 0, abort);
}
