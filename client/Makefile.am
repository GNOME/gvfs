NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

module_flags = -export_dynamic -avoid-version -module -no-undefined
modulesdir = $(libdir)/gio/gvfs-modules

modules_LTLIBRARIES = libgvfsdbus.la

libgvfsdbus_la_LDFLAGS = $(module_flags)
libgvfsdbus_la_SOURCES = 	\
	gsysutils.c gsysutils.h \
	gdbusutils.c gdbusutils.h \
	gvfsimpldaemon.c gvfsimpldaemon.h \
	gfiledaemon.c gfiledaemon.h \
	gfiledaemonlocal.c gfiledaemonlocal.h \
	gvfsuriutils.c gvfsuriutils.h \
	gfileinputstreamdaemon.c gfileinputstreamdaemon.h \
	gfileenumeratordaemon.c gfileenumeratordaemon.h \
	gvfsdaemondbus.c \
	$(NULL)

libgvfsdbus_la_LIBADD = \
	$(top_builddir)/gio/libgio.la	\
	$(DBUS_LIBS) 			\
	$(GLIB_LIBS)
