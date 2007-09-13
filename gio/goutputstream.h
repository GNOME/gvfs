#ifndef __G_OUTPUT_STREAM_H__
#define __G_OUTPUT_STREAM_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gioerror.h>
#include <gio/gasyncresult.h>
#include <gio/gcancellable.h>

G_BEGIN_DECLS

#define G_TYPE_OUTPUT_STREAM         (g_output_stream_get_type ())
#define G_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_OUTPUT_STREAM, GOutputStream))
#define G_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_OUTPUT_STREAM, GOutputStreamClass))
#define G_IS_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_OUTPUT_STREAM))
#define G_IS_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_OUTPUT_STREAM))
#define G_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_OUTPUT_STREAM, GOutputStreamClass))

typedef struct _GOutputStream         GOutputStream;
typedef struct _GOutputStreamClass    GOutputStreamClass;
typedef struct _GOutputStreamPrivate  GOutputStreamPrivate;

struct _GOutputStream
{
  GObject parent;
  
  /*< private >*/
  GOutputStreamPrivate *priv;
};


struct _GOutputStreamClass
{
  GObjectClass parent_class;

  /* Sync ops: */
  
  gssize      (* write)  (GOutputStream *stream,
			  void *buffer,
			  gsize count,
			  GCancellable *cancellable,
			  GError **error);
  gboolean    (* flush)	 (GOutputStream *stream,
			  GCancellable  *cancellable,
			  GError       **error);
  gboolean    (* close)	 (GOutputStream *stream,
			  GCancellable  *cancellable,
			  GError       **error);

  /* Async ops: (optional in derived classes) */

  void     (* write_async)  (GOutputStream       *stream,
			     void                *buffer,
			     gsize                count,
			     int                  io_priority,
			     GCancellable        *cancellable,
			     GAsyncReadyCallback  callback,
			     gpointer             user_data);
  gssize   (* write_finish) (GOutputStream       *stream,
			     GAsyncResult        *result,
			     GError             **error);
  void     (* flush_async)  (GOutputStream       *stream,
			     int                  io_priority,
			     GCancellable        *cancellable,
			     GAsyncReadyCallback  callback,
			     gpointer             user_data);
  gboolean (* flush_finish) (GOutputStream       *stream,
			     GAsyncResult        *result,
			     GError             **error);
  void     (* close_async)  (GOutputStream       *stream,
			     int                  io_priority,
			     GCancellable        *cancellable,
			     GAsyncReadyCallback  callback,
			     gpointer             user_data);
  gboolean (* close_finish) (GOutputStream       *stream,
			     GAsyncResult        *result,
			     GError             **error);

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_output_stream_get_type (void) G_GNUC_CONST;
  
gssize   g_output_stream_write        (GOutputStream        *stream,
				       void                 *buffer,
				       gsize                 count,
				       GCancellable         *cancellable,
				       GError              **error);
gboolean g_output_stream_write_all    (GOutputStream        *stream,
				       void                 *buffer,
				       gsize                 count,
				       gsize                *bytes_written,
				       GCancellable         *cancellable,
				       GError              **error);
gboolean g_output_stream_flush        (GOutputStream        *stream,
				       GCancellable         *cancellable,
				       GError              **error);
gboolean g_output_stream_close        (GOutputStream        *stream,
				       GCancellable         *cancellable,
				       GError              **error);
void     g_output_stream_write_async  (GOutputStream        *stream,
				       void                 *buffer,
				       gsize                 count,
				       int                   io_priority,
				       GCancellable         *cancellable,
				       GAsyncReadyCallback   callback,
				       gpointer              user_data);
gssize   g_output_stream_write_finish (GOutputStream        *stream,
				       GAsyncResult         *result,
				       GError              **error);
void     g_output_stream_flush_async  (GOutputStream        *stream,
				       int                   io_priority,
				       GCancellable         *cancellable,
				       GAsyncReadyCallback   callback,
				       gpointer              user_data);
gboolean g_output_stream_flush_finish (GOutputStream        *stream,
				       GAsyncResult         *result,
				       GError              **error);
void     g_output_stream_close_async  (GOutputStream        *stream,
				       int                   io_priority,
				       GCancellable         *cancellable,
				       GAsyncReadyCallback   callback,
				       gpointer              user_data);
gboolean g_output_stream_close_finish (GOutputStream        *stream,
				       GAsyncResult         *result,
				       GError              **error);
gboolean g_output_stream_is_closed    (GOutputStream        *stream);
gboolean g_output_stream_has_pending  (GOutputStream        *stream);
void     g_output_stream_set_pending  (GOutputStream        *stream,
				       gboolean              pending);

G_END_DECLS

#endif /* __G_OUTPUT_STREAM_H__ */
