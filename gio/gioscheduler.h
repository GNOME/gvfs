#ifndef __G_IO_SCHEDULER_H__
#define __G_IO_SCHEDULER_H__

#include <glib.h>
#include <gcancellable.h>

G_BEGIN_DECLS

typedef struct _GIOJob GIOJob;

typedef void (*GIOJobFunc) (GIOJob *job,
			    GCancellable *cancellable,
			    gpointer data);

typedef void (*GIODataFunc) (gpointer data);

void g_schedule_io_job         (GIOJobFunc      job_func,
				gpointer        data,
				GDestroyNotify  notify,
				gint            io_priority,
				GCancellable   *cancellable);
void g_cancel_all_io_jobs      (void);

void g_io_job_send_to_mainloop (GIOJob         *job,
				GIODataFunc     func,
				gpointer        data,
				GDestroyNotify  notify,
				gboolean        block);


G_END_DECLS

#endif /* __G_IO_SCHEDULER_H__ */
