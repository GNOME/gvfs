NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	$(GLIB_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED

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

libgvfs_la_SOURCES = 	\
	gasynchelper.c \
	gasynchelper.h \
	gfile.c \
	gfileenumerator.c \
	gfileenumeratorsimple.c \
	gfileenumeratorsimple.h \
	gfileinfo.c \
	gfileinfosimple.c \
	gfileinfosimple.h \
	gfileinputstream.c \
	gfileoutputstream.c \
	gfilesimple.c \
	gfilesimple.h \
	ginputstream.c \
	gioscheduler.c \
	glocalfileinputstream.c \
	glocalfileinputstream.h \
	glocalfileoutputstream.c \
	glocalfileoutputstream.h \
	goutputstream.c \
	gvfs.c \
	gvfs.h \
	gvfserror.c \
	gvfssimple.c \
	gvfssimple.h \
	gmountoperation.c \
	gmountoperation.h \
	$(marshal_sources) \
	$(NULL)

libgvfs_la_LIBADD = \
	$(SELINUX_LIBS) \
	$(GLIB_LIBS)

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

