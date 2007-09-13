#include <config.h>

#include <unistd.h>
#include <fcntl.h>

#include <giotypes.h>
#include <gioerror.h>
#include <glib/gi18n-lib.h>

#include "gcancellable.h"

enum {
  CANCELLED,
  LAST_SIGNAL
};

struct _GCancellable
{
  GObject parent_instance;

  guint cancelled : 1;
  guint allocated_pipe : 1;
  int cancel_pipe[2];
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GCancellable, g_cancellable, G_TYPE_OBJECT);

static GStaticPrivate current_cancellable = G_STATIC_PRIVATE_INIT;
G_LOCK_DEFINE_STATIC(cancellable);
  
static void
g_cancellable_finalize (GObject *object)
{
  GCancellable *cancellable = G_CANCELLABLE (object);

  if (cancellable->cancel_pipe[0] != -1)
    close (cancellable->cancel_pipe[0]);
  
  if (cancellable->cancel_pipe[1] != -1)
    close (cancellable->cancel_pipe[1]);
  
  if (G_OBJECT_CLASS (g_cancellable_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_cancellable_parent_class)->finalize) (object);
}

static void
g_cancellable_class_init (GCancellableClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_cancellable_finalize;

  signals[CANCELLED] =
    g_signal_new (I_("cancelled"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GCancellableClass, cancelled),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  
}

static void
set_fd_nonblocking (int fd)
{
#ifdef F_GETFL
  glong fcntl_flags;
  fcntl_flags = fcntl (fd, F_GETFL);

#ifdef O_NONBLOCK
  fcntl_flags |= O_NONBLOCK;
#else
  fcntl_flags |= O_NDELAY;
#endif

  fcntl (fd, F_SETFL, fcntl_flags);
#endif
}

static void
g_cancellable_open_pipe (GCancellable *cancellable)
{
  if (pipe (cancellable->cancel_pipe) == 0)
    {
      /* Make them nonblocking, just to be sure we don't block
       * on errors and stuff
       */
      set_fd_nonblocking (cancellable->cancel_pipe[0]);
      set_fd_nonblocking (cancellable->cancel_pipe[1]);
    }
}

static void
g_cancellable_init (GCancellable *cancellable)
{
  cancellable->cancel_pipe[0] = -1;
  cancellable->cancel_pipe[1] = -1;
}

GCancellable *
g_cancellable_new (void)
{
  return g_object_new (G_TYPE_CANCELLABLE, NULL);
}

void
g_push_current_cancellable (GCancellable *cancellable)
{
  GSList *l;
  
  l = g_static_private_get (&current_cancellable);
  l = g_slist_prepend (l, cancellable);
  g_static_private_set (&current_cancellable, l, NULL);
}

void
g_pop_current_cancellable (GCancellable *cancellable)
{
  GSList *l;
  
  l = g_static_private_get (&current_cancellable);
  
  g_assert (l != NULL);
  g_assert (l->data == cancellable);

  l = g_slist_delete_link (l, l);
  g_static_private_set (&current_cancellable, l, NULL);
}


GCancellable *
g_get_current_cancellable  (void)
{
  GSList *l;
  
  l = g_static_private_get (&current_cancellable);
  if (l == NULL)
    return NULL;

  return G_CANCELLABLE (l->data);
}

void 
g_cancellable_reset (GCancellable *cancellable)
{
  G_LOCK(cancellable);
  /* Make sure we're not leaving old cancel state around */
  if (cancellable->cancelled)
    {
      char ch;
      if (cancellable->cancel_pipe[0] != -1)
	read (cancellable->cancel_pipe[0], &ch, 1);
      cancellable->cancelled = FALSE;
    }
  G_UNLOCK(cancellable);
}

/* Safe to call with NULL */
gboolean
g_cancellable_is_cancelled (GCancellable *cancellable)
{
  return cancellable != NULL && cancellable->cancelled;
}

gboolean
g_cancellable_set_error_if_cancelled (GCancellable  *cancellable,
				      GError       **error)
{
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return TRUE;
    }
  
  return FALSE;
}

/* May return -1 if fds not supported, or on errors */
int
g_cancellable_get_fd (GCancellable *cancellable)
{
  int fd;
  if (cancellable == NULL)
    return -1;
  
  G_LOCK(cancellable);
  if (!cancellable->allocated_pipe)
    {
      cancellable->allocated_pipe = TRUE;
      g_cancellable_open_pipe (cancellable);
    }
  
  fd = cancellable->cancel_pipe[0];
  G_UNLOCK(cancellable);
  
  return fd;
}

/* This is safe to call from another thread */
void
g_cancellable_cancel (GCancellable *cancellable)
{
  G_LOCK(cancellable);
  if (cancellable != NULL &&
      !cancellable->cancelled)
    {
      char ch = 'x';
      cancellable->cancelled = TRUE;
      if (cancellable->cancel_pipe[1] != -1)
	write (cancellable->cancel_pipe[1], &ch, 1);
    }
  G_UNLOCK(cancellable);
  
  g_object_ref (cancellable);
  g_signal_emit (cancellable, signals[CANCELLED], 0);
  g_object_unref (cancellable);
}


