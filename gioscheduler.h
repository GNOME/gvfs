#ifndef __G_IO_SCHEDULER_H__
#define __G_IO_SCHEDULER_H__

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GIOJob GIOJob;

typedef void (*GIOJobFunc) (GIOJob *job,
			    gpointer data);

typedef void (*GIODataFunc) (gpointer data);

gint g_schedule_io_job (GIOJobFunc    job_func,
			GIODataFunc   cancel_func,
			gpointer      data,
			GDestroyNotify notify,
			gint          io_priority,
			GMainContext *callback_context);
void g_cancel_io_job (gint id);
void g_cancel_all_io_jobs (void);


void g_io_job_send_to_mainloop (GIOJob     *job,
				GIODataFunc func,
				gpointer    data,
				GDestroyNotify notify,
				gboolean    block);
gboolean g_io_job_is_cancelled (GIOJob *job);

G_END_DECLS

#endif /* __G_IO_SCHEDULER_H__ */
