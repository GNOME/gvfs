#ifndef __MAIN_HELPER_H__
#define __MAIN_HELPER_H__

#include "gmountsource.h"

G_BEGIN_DECLS

void          daemon_init       (void);
GMountSpec   *daemon_parse_args (int         argc,
				 char       *argv[],
				 const char *default_type);
void          daemon_main       (int         argc,
				 char       *argv[],
				 int max_job_threads,
				 const char *default_type,
				 const char *mountable_name,
				 const char *first_type_name,
				 ...);

G_END_DECLS

#endif /* __MAIN_HELPER__ */
