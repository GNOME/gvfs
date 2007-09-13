NULL =

INCLUDES = -I$(top_srcdir) \
	$(GLIB_CFLAGS) \
	-DG_DISABLE_DEPRECATED

noinst_LTLIBRARIES = libinotify.la

libinotify_la_SOURCES = 		\
	inotify-kernel.c 		\
	inotify-sub.c 			\
	inotify-path.c 			\
	inotify-missing.c 		\
	inotify-helper.c 		\
	inotify-diag.c			\
	inotify-diag.h			\
	inotify-kernel.h 		\
	inotify-missing.h		\
	inotify-path.h			\
	inotify-sub.h			\
	inotify-helper.h		\
	local_inotify.h			\
	local_inotify_syscalls.h	\
	$(NULL)

