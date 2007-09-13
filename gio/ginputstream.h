#ifndef __G_INPUT_STREAM_H__
#define __G_INPUT_STREAM_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gioerror.h>
#include <gio/gcancellable.h>
#include <gio/gasyncresult.h>

G_BEGIN_DECLS

#define G_TYPE_INPUT_STREAM         (g_input_stream_get_type ())
#define G_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_INPUT_STREAM, GInputStream))
#define G_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_INPUT_STREAM, GInputStreamClass))
#define G_IS_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_INPUT_STREAM))
#define G_IS_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_INPUT_STREAM))
#define G_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_INPUT_STREAM, GInputStreamClass))

typedef struct _GInputStream         GInputStream;
typedef struct _GInputStreamClass    GInputStreamClass;
typedef struct _GInputStreamPrivate  GInputStreamPrivate;

struct _GInputStream
{
  GObject parent;

  /*< private >*/
  GInputStreamPrivate *priv;
};

struct _GInputStreamClass
{
  GObjectClass parent_class;

  /* Sync ops: */
  
  gssize   (* read)        (GInputStream *stream,
			    void         *buffer,
			    gsize         count,
			    GCancellable *cancellable,
			    GError      **error);
  gssize   (* skip)        (GInputStream *stream,
			    gsize         count,
			    GCancellable *cancellable,
			    GError      **error);
  gboolean (* close)	   (GInputStream *stream,
			    GCancellable *cancellable,
			    GError      **error);

  /* Async ops: (optional in derived classes) */
  void     (* read_async)  (GInputStream        *stream,
			    void               *buffer,
			    gsize               count,
			    int                 io_priority,
			    GCancellable       *cancellable,
			    GAsyncReadyCallback callback,
			    gpointer            user_data);
  gssize   (* read_finish) (GInputStream       *stream,
			    GAsyncResult       *result,
			    GError            **error);
  void     (* skip_async)  (GInputStream       *stream,
			    gsize               count,
			    int                 io_priority,
			    GCancellable       *cancellable,
			    GAsyncReadyCallback callback,
			    gpointer            user_data);
  gssize   (* skip_finish) (GInputStream        *stream,
			    GAsyncResult       *result,
			    GError            **error);
  void     (* close_async) (GInputStream        *stream,
			    int                  io_priority,
			    GCancellable       *cancellable,
			    GAsyncReadyCallback callback,
			    gpointer            user_data);
  gboolean (* close_finish)(GInputStream        *stream,
			    GAsyncResult       *result,
			    GError            **error);

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_input_stream_get_type (void) G_GNUC_CONST;

gssize   g_input_stream_read         (GInputStream          *stream,
				      void                  *buffer,
				      gsize                  count,
				      GCancellable          *cancellable,
				      GError               **error);
gboolean g_input_stream_read_all     (GInputStream          *stream,
				      void                  *buffer,
				      gsize                  count,
				      gsize                 *bytes_read,
				      GCancellable          *cancellable,
				      GError               **error);
gssize   g_input_stream_skip         (GInputStream          *stream,
				      gsize                  count,
				      GCancellable          *cancellable,
				      GError               **error);
gboolean g_input_stream_close        (GInputStream          *stream,
				      GCancellable          *cancellable,
				      GError               **error);
void     g_input_stream_read_async   (GInputStream          *stream,
				      void                  *buffer,
				      gsize                  count,
				      int                    io_priority,
				      GCancellable          *cancellable,
				      GAsyncReadyCallback    callback,
				      gpointer               user_data);
gssize   g_input_stream_read_finish  (GInputStream          *stream,
				      GAsyncResult          *result,
				      GError               **error);
void     g_input_stream_skip_async   (GInputStream          *stream,
				      gsize                  count,
				      int                    io_priority,
				      GCancellable          *cancellable,
				      GAsyncReadyCallback    callback,
				      gpointer               user_data);
gssize   g_input_stream_skip_finish  (GInputStream          *stream,
				      GAsyncResult          *result,
				      GError               **error);
void     g_input_stream_close_async  (GInputStream          *stream,
				      int                    io_priority,
				      GCancellable          *cancellable,
				      GAsyncReadyCallback    callback,
				      gpointer               user_data);
gboolean g_input_stream_close_finish (GInputStream          *stream,
				      GAsyncResult          *result,
				      GError               **error);

/* For implementations: */

gboolean g_input_stream_is_closed    (GInputStream          *stream);
gboolean g_input_stream_has_pending  (GInputStream          *stream);
void     g_input_stream_set_pending  (GInputStream          *stream,
				      gboolean               pending);

G_END_DECLS

#endif /* __G_INPUT_STREAM_H__ */
