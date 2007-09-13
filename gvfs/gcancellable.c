#include <config.h>

#include <unistd.h>
#include <fcntl.h>

#include "gcancellable.h"

struct _GCancellable
{
  GObject parent_instance;

  int active_count;
  int cancel_count;
  int cancel_pipe[2];
};

G_DEFINE_TYPE (GCancellable, g_cancellable, G_TYPE_OBJECT);

static GStaticPrivate current_cancellable = G_STATIC_PRIVATE_INIT;
G_LOCK_DEFINE_STATIC(cancellable);
  
static void
g_cancellable_finalize (GObject *object)
{
  GCancellable *cancellable;

  cancellable = G_CANCELLABLE (object);

  g_assert (cancellable->active_count == 0);
  
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
}

static void
set_fd_nonblocking (int fd)
{
  glong fcntl_flags;

  fcntl_flags = fcntl (fd, F_GETFL);

#ifdef O_NONBLOCK
  fcntl_flags |= O_NONBLOCK;
#else
  fcntl_flags |= O_NDELAY;
#endif

  fcntl (fd, F_SETFL, fcntl_flags);
}

static void
g_cancellable_init (GCancellable *cancellable)
{
  if (pipe (cancellable->cancel_pipe) == 0)
    {
      /* Make them nonblocking, just to be sure we don't block
       * on errors and stuff
       */
      set_fd_nonblocking (cancellable->cancel_pipe[0]);
      set_fd_nonblocking (cancellable->cancel_pipe[1]);
    }
  else
    {
      cancellable->cancel_pipe[0] = -1;
      cancellable->cancel_pipe[1] = -1;
    }
}

GCancellable *
g_get_current_cancellable  (void)
{
  GCancellable *c;
  
  c = g_static_private_get (&current_cancellable);
  if (c == NULL)
    {
      c = g_object_new (G_TYPE_CANCELLABLE, NULL);
      g_static_private_set (&current_cancellable, c, (GDestroyNotify)g_object_unref);
    }
  
  return c;
}

/* These are only safe to call inside a cancellable op */
GCancellable *
g_cancellable_begin (GCancellableOp cancellable_op)
{
  GCancellable *c;

  c = g_get_current_cancellable ();

  G_LOCK(cancellable);
  if (c->active_count++ == 0)
    {
      char ch;
      /* Non-recursive */

      /* Make sure we're not leaving old cancel state around */
      while (c->cancel_count > 0)
	{
	  if (c->cancel_pipe[0] != -1)
	    read (c->cancel_pipe[0], &ch, 1);
	  c->cancel_count--;
	}
    }
  *(GCancellable **)cancellable_op = c;
  
  G_UNLOCK(cancellable);

  return c;
}

void
g_cancellable_end (GCancellableOp  cancellable_op)
{
  GCancellable *c;

  /* Safe to do outside lock, since we're between begin/end */
  c = *(GCancellable **)cancellable_op;
  g_assert (c != NULL);

  G_LOCK(cancellable);
  
  c->active_count--;
  /* cancel_count is cleared on begin */

  *(GCancellable **)cancellable_op = NULL;
  
  G_UNLOCK(cancellable);
}

gboolean
g_cancellable_is_cancelled (GCancellable *cancellable)
{
  return cancellable->cancel_count != 0;
}

/* May return -1 if fds not supported, or on errors */
int
g_cancellable_get_fd (GCancellable *cancellable)
{
  return cancellable->cancel_pipe[0];
}

/* This is safe to call from another thread */
void
g_cancellable_cancel (GCancellableOp  cancellable_op)
{
  GCancellable *c;

  G_LOCK(cancellable);
  
  c = *(GCancellable **)cancellable_op;
  if (c != NULL)
    {
      char ch = 'x';
      
      c->cancel_count++;
      if (c->cancel_pipe[1] != -1)
	write (c->cancel_pipe[1], &ch, 1);
    }
  
  G_UNLOCK(cancellable);
}


