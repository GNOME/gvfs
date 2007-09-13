NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	$(GLIB_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED

lib_LTLIBRARIES = libgvfs.la

libgvfs_la_SOURCES = 	\
	gasynchelper.c \
	gasynchelper.h \
	gfile.c \
	gfileenumerator.c \
	gfileinfo.c \
	gfileinputstream.c \
	gfileoutputstream.c \
	gioscheduler.c \
	glocalfileinputstream.c \
	glocalfileoutputstream.c \
	goutputstream.c \
	ginputstream.c \
	gvfs.c \
	gvfserror.c \
	gvfs.h \
	gfileenumeratorsimple.c \
	gfileenumeratorsimple.h \
	gfileinfosimple.c \
	gfileinfosimple.h \
	gfilesimple.c \
	gfilesimple.h \
	gvfssimple.c \
	gvfssimple.h \
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
	glocalfileinputstream.h \
	glocalfileoutputstream.h \
	$(NULL)

noinst_PROGRAMS= \
	test

test_SOURCES = \
	test.c

test_LDADD = \
	libgvfs.la \
	$(GLIB_LIBS)
