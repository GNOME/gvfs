NULL =

AM_CFLAGS =                       \
	-I$(top_srcdir)           \
	-I$(top_builddir)         \
	$(GLIB_CFLAGS)            \
	-DG_DISABLE_DEPRECATED

AM_LDFLAGS =                           \
	$(GLIB_LIBS)

noinst_PROGRAMS = \
	test-query-info-stream    \
	benchmark-gvfs-small-files    \
	benchmark-gvfs-big-files      \
	benchmark-posix-small-files   \
	benchmark-posix-big-files     \
	$(NULL)

EXTRA_DIST = benchmark-common.c
