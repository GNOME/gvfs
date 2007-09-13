NULL =

noinst_LTLIBRARIES=libcommon.la

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

libcommon_la_SOURCES = 	\
	gsysutils.c gsysutils.h \
	gdbusutils.c gdbusutils.h \
	gmountspec.c gmountspec.h \
	gmountoperationdbus.c gmountoperation.h \
	gmountsource.c gmountsource.h \
	gvfsdaemonprotocol.h \
	$(NULL)

libcommon_la_LIBADD =	\
	$(DBUS_LIBS) 	\
	$(GLIB_LIBS)
