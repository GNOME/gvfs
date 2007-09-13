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

if OS_UNIX
appinfo_sources += gdesktopappinfo.c gdesktopappinfo.h
platform_libadd += xdgmime/libxdgmime.la
socket_sources = \
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
	gcancellable.c \
	gcontenttype.c \
	gcontenttypeprivate.h \
	gfile.c \
	gfileenumerator.c \
	gfileinfo.c \
	gfileinputstream.c \
	gfileoutputstream.c \
	ginputstream.c \
	gioerror.c \
	gioscheduler.c \
	gmountoperation.c \
	gmountoperation.h \
	goutputstream.c \
	gseekable.c \
	gvfs.c \
	$(appinfo_sources) \
	$(socket_sources) \
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

libgio_la_LDFLAGS= -export-dynamic $(no_undefined)

gvfsincludedir=$(includedir)/gio/
gvfsinclude_HEADERS = \
	gappinfo.h \
	gcancellable.h \
	gcontenttype.h \
	gfile.h \
	gfileenumerator.h \
	gfileinfo.h \
	gfileinputstream.h \
	gfileoutputstream.h \
	ginputstream.h \
	gioerror.h \
	gioscheduler.h \
	giotypes.h \
	goutputstream.h \
	gseekable.h \
	gvfs.h \
	$(NULL)

noinst_PROGRAMS= \
	test-gio

test_gio_SOURCES = \
	test-gio.c

test_gio_LDADD = \
	libgio.la \
	$(GLIB_LIBS)

EXTRA_DIST = 				\
	gvfs-marshal.list		\
	$(NULL)

CLEANFILES = \
	$(marshal_sources)				\
	$(NULL)
