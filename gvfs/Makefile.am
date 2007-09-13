NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

lib_LTLIBRARIES = libgvfs.la

marshal_sources = \
        gvfs-marshal.h \
        gvfs-marshal.c \
	$(NULL)

gvfs-marshal.h: gvfs-marshal.list $(GLIB_GENMARSHAL)
	$(GLIB_GENMARSHAL) --prefix=_gvfs_marshal $(srcdir)/gvfs-marshal.list --header > $@

gvfs-marshal.c: gvfs-marshal.h gvfs-marshal.list $(GLIB_GENMARSHAL)
	(echo "#include \"gvfs-marshal.h\""; \
	 $(GLIB_GENMARSHAL) --prefix=_gvfs_marshal $(srcdir)/gvfs-marshal.list --body) > $@

local_sources = \
	gfileenumeratorlocal.c \
	gfileenumeratorlocal.h \
	gfileinfolocal.c \
	gfileinfolocal.h \
	gfilelocal.c \
	gfilelocal.h \
	gfileinputstreamlocal.c \
	gfileinputstreamlocal.h \
	gfileoutputstreamlocal.c \
	gfileoutputstreamlocal.h \
	gvfslocal.c \
	gvfslocal.h \
	$(NULL)

daemon_sources = \
	gvfsimpldaemon.c gvfsimpldaemon.h \
	gfiledaemon.c gfiledaemon.h \
	gfiledaemonlocal.c gfiledaemonlocal.h \
	gvfsuriutils.c gvfsuriutils.h \
	gfileinputstreamdaemon.c gfileinputstreamdaemon.h \
	gfileenumeratordaemon.c gfileenumeratordaemon.h \
	gvfsdaemondbus.c \
	$(NULL)

libgvfs_la_SOURCES = 	\
	gasynchelper.c \
	gasynchelper.h \
	gcancellable.c \
	gcancellable.h \
	gfile.c \
	gfileenumerator.c \
	gfileinfo.c \
	gfileinputstream.c \
	gfileoutputstream.c \
	gseekable.c \
	ginputstreamsocket.c \
	ginputstreamsocket.h \
	goutputstreamsocket.c \
	goutputstreamsocket.h \
	ginputstream.c \
	gioscheduler.c \
	gmountoperation.c \
	gmountoperation.h \
	goutputstream.c \
	gvfs.c \
	gvfs.h \
	gvfserror.c \
	$(local_sources) \
	$(daemon_sources) \
	$(marshal_sources) \
	$(NULL)

$(libgvfs_la_OBJECTS): $(marshal_sources)

libgvfs_la_LIBADD = \
	$(SELINUX_LIBS) \
	$(DBUS_LIBS) \
	$(GLIB_LIBS) \
	$(XATTR_LIBS)

gvfsincludedir=$(includedir)/gvfs/
gvfsinclude_HEADERS = \
	gfile.h \
	gfileenumerator.h \
	gfileinfo.h \
	gfileinputstream.h \
	gfileoutputstream.h \
	ginputstream.h \
	gioscheduler.h \
	goutputstream.h \
	gseekable.h \
	gvfserror.h \
	gvfstypes.h \
	$(NULL)

noinst_PROGRAMS= \
	test

test_SOURCES = \
	test.c

test_LDADD = \
	libgvfs.la \
	$(GLIB_LIBS)

EXTRA_DIST = 				\
	gvfs-marshal.list		\
	$(NULL)

CLEANFILES = \
	$(marshal_sources)				\
	$(NULL)

