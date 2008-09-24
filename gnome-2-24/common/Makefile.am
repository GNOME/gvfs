NULL =

lib_LTLIBRARIES=libgvfscommon.la

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

libgvfscommon_la_SOURCES = 	\
	gsysutils.c gsysutils.h \
	gdbusutils.c gdbusutils.h \
	gmountspec.c gmountspec.h \
	gmountoperationdbus.c gmountoperationdbus.h \
	gmountsource.c gmountsource.h \
	gmounttracker.c gmounttracker.h \
	gvfsdaemonprotocol.c gvfsdaemonprotocol.h \
	$(NULL)

libgvfscommon_la_LIBADD =	\
	$(DBUS_LIBS) 	\
	$(GLIB_LIBS)
