#ifndef __G_CANCELLABLE_H__
#define __G_CANCELLABLE_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_CANCELLABLE         (g_cancellable_get_type ())
#define G_CANCELLABLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_CANCELLABLE, GCancellable))
#define G_CANCELLABLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_CANCELLABLE, GCancellableClass))
#define G_IS_CANCELLABLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_CANCELLABLE))
#define G_IS_CANCELLABLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_CANCELLABLE))
#define G_CANCELLABLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_CANCELLABLE, GCancellableClass))

typedef struct _GCancellable        GCancellable;
typedef struct _GCancellableClass   GCancellableClass;

struct _GCancellableClass
{
  GObjectClass parent_class;

  void (* cancelled) (GCancellable *cancellable);
};

GType g_cancellable_get_type (void) G_GNUC_CONST;

GCancellable *g_cancellable_new          (void);

/* These are only safe to call inside a cancellable op */
gboolean      g_cancellable_is_cancelled (GCancellable *cancellable);
int           g_cancellable_get_fd       (GCancellable *cancellable);
GCancellable *g_get_current_cancellable  (void);
void          g_push_current_cancellable (GCancellable *cancellable);
void          g_pop_current_cancellable  (GCancellable *cancellable);
void          g_cancellable_reset        (GCancellable *cancellable);

/* This is safe to call from another thread */
void          g_cancellable_cancel       (GCancellable  *cancellable);

G_END_DECLS

#endif /* __G_CANCELLABLE_H__ */
