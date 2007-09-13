NULL =

SUBDIRS=

if OS_UNIX
SUBDIRS += xdgmime
endif

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	-DGIO_MODULE_DIR=\"$(libdir)/gio/gvfs-modules\"	\
	$(GLIB_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

lib_LTLIBRARIES = libgio.la

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
	glocaldirectorymonitor.c \
	glocaldirectorymonitor.h \
	glocalfilemonitor.c \
	glocalfilemonitor.h \
	glocalfileenumerator.c \
	glocalfileenumerator.h \
	glocalfileinfo.c \
	glocalfileinfo.h \
	glocalfile.c \
	glocalfile.h \
	glocalfileinputstream.c \
	glocalfileinputstream.h \
	glocalfileoutputstream.c \
	glocalfileoutputstream.h \
	glocalvfs.c \
	glocalvfs.h \
	$(NULL)

platform_libadd =
appinfo_sources =

if HAVE_INOTIFY
SUBDIRS += inotify
platform_libadd += inotify/libinotify.la
endif

if HAVE_FAM
SUBDIRS += fam
platform_libadd += fam/libfam_helper.la
endif

if OS_UNIX
appinfo_sources += gdesktopappinfo.c gdesktopappinfo.h
platform_libadd += xdgmime/libxdgmime.la
unix_sources = \
	gunixmounts.c \
	gunixmounts.h \
	gunixdrive.c \
	gunixdrive.h \
	gunixvolume.c \
	gunixvolume.h \
	gunixvolumemonitor.c \
	gunixvolumemonitor.h \
	gsocketinputstream.c \
	gsocketinputstream.h \
	gsocketoutputstream.c \
	gsocketoutputstream.h \
	$(NULL)
endif

if OS_WIN32
appinfo_sources += gwin32appinfo.c gwin32appinfo.h
platform_libadd += -lshlwapi
endif

libgio_la_SOURCES = 	\
	gappinfo.c \
	gasynchelper.c \
	gasynchelper.h \
	gasyncresult.c \
	gbufferedinputstream.c \
	gbufferedoutputstream.c \
	gcancellable.c \
	gcontenttype.c \
	gcontenttypeprivate.h \
	gdirectorymonitor.c \
	gdrive.c \
	gdrivepriv.h \
	gdummyfile.c \
	gfile.c \
	gfileenumerator.c \
	gfileicon.c \
	gfileattribute.c \
	gfileinfo.c \
	gfileinputstream.c \
	gfilemonitor.c \
	gfileoutputstream.c \
	gfilterinputstream.c \
	gfilteroutputstream.c \
	gicon.c \
	ginputstream.c \
	gioerror.c \
	giomodule.c \
	gioscheduler.c \
	gloadableicon.c \
	gmemoryinputstream.c \
	gmemoryoutputstream.c \
	gmountoperation.c \
	goutputstream.c \
	gpollfilemonitor.c \
	gpollfilemonitor.h \
	gseekable.c \
	gsimpleasyncresult.c \
	gsimpleasyncresult.h \
	gthemedicon.c \
	guniondrive.c \
	guniondrive.h \
	gunionvolume.c \
	gunionvolume.h \
	gunionvolumemonitor.c \
	gunionvolumemonitor.h \
	gvfs.c \
	gvolume.c \
	gvolumemonitor.c \
	gvolumepriv.h \
	$(appinfo_sources) \
	$(unix_sources) \
	$(local_sources) \
	$(daemon_sources) \
	$(marshal_sources) \
	$(NULL)

$(libgio_la_OBJECTS): $(marshal_sources)

libgio_la_LIBADD = \
	$(platform_libadd) \
	$(SELINUX_LIBS) \
	$(GLIB_LIBS) \
	$(XATTR_LIBS) \
	$(NULL)

if OS_WIN32
no_undefined = -no-undefined
endif

if HAVE_FAM
fam_ldflags = $(FAM_LIBS)
endif

libgio_la_LDFLAGS= -export-dynamic $(no_undefined) $(fam_ldflags)

gvfsincludedir=$(includedir)/gio/
gvfsinclude_HEADERS = \
	gappinfo.h \
	gasyncresult.h \
	gbufferedinputstream.h \
	gbufferedoutputstream.h \
	gcancellable.h \
	gcontenttype.h \
	gdirectorymonitor.h \
	gdrive.h \
	gdummyfile.h \
	gfile.h \
	gfileenumerator.h \
	gfileicon.h \
	gfileattribute.h \
	gfileinfo.h \
	gfileinputstream.h \
	gfilemonitor.h \
	gfileoutputstream.h \
	gfilterinputstream.h \
	gfilteroutputstream.h \
	gicon.h \
	ginputstream.h \
	gioerror.h \
	giomodule.h \
	gioscheduler.h \
	giotypes.h \
	gloadableicon.h \
	gmemoryinputstream.h \
	gmemoryoutputstream.h \
	gmountoperation.h \
	goutputstream.h \
	gseekable.h \
	gthemedicon.h \
	gvfs.h \
	gvolume.h \
	gvolumemonitor.h \
	$(NULL)

noinst_PROGRAMS= \
	test-gio \
	test-streams

test_gio_SOURCES = \
	test-gio.c

test_gio_LDADD = \
	libgio.la \
	$(GLIB_LIBS)

test_streams_SOURCES = \
	test-streams.c

test_streams_LDADD = \
	libgio.la \
	$(GLIB_LIBS)

if OS_WIN32
else
export_symbols = $(LIBTOOL_EXPORT_OPTIONS)
endif

EXTRA_DIST = 				\
	gvfs-marshal.list		\
	$(NULL)

CLEANFILES = \
	$(marshal_sources)				\
	$(NULL)
