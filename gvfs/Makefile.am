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

simple_sources = \
	gfileenumeratorsimple.c \
	gfileenumeratorsimple.h \
	gfileinfosimple.c \
	gfileinfosimple.h \
	gfilesimple.c \
	gfilesimple.h \
	glocalfileinputstream.c \
	glocalfileinputstream.h \
	glocalfileoutputstream.c \
	glocalfileoutputstream.h \
	gvfssimple.c \
	gvfssimple.h \
	$(NULL)

unix_sources = \
	gvfsunix.c gvfsunix.h \
	gfileunix.c gfileunix.h \
	gfileunixsimple.c gfileunixsimple.h \
	gvfsunixuri.c gvfsunixuri.h \
	gunixfileinputstream.c gunixfileinputstream.h \
	gvfsunixdbus.c \
	$(NULL)

libgvfs_la_SOURCES = 	\
	gasynchelper.c \
	gasynchelper.h \
	gfile.c \
	gfileenumerator.c \
	gfileinfo.c \
	gfileinputstream.c \
	gfileoutputstream.c \
	ginputstream.c \
	gioscheduler.c \
	gmountoperation.c \
	gmountoperation.h \
	goutputstream.c \
	gvfs.c \
	gvfs.h \
	gvfserror.c \
	$(simple_sources) \
	$(unix_sources) \
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

