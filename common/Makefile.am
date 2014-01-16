NULL =

pkglib_LTLIBRARIES=libgvfscommon.la
noinst_LTLIBRARIES = libgvfscommon-monitor.la

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(BLURAY_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\"

dbus_built_sources = gvfsdbus.c gvfsdbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources) : Makefile.am org.gtk.vfs.xml
	$(AM_V_GEN) gdbus-codegen			\
		--interface-prefix org.gtk.vfs.		\
		--c-namespace GVfsDBus			\
		--generate-c-code gvfsdbus		\
		$(srcdir)/org.gtk.vfs.xml		\
		$(NULL)

libgvfscommon_la_SOURCES = 	\
	gmountspec.c gmountspec.h \
	gmountoperationdbus.c gmountoperationdbus.h \
	gmountsource.c gmountsource.h \
	gmounttracker.c gmounttracker.h \
	gvfsdaemonprotocol.c gvfsdaemonprotocol.h \
	gvfsicon.h gvfsicon.c \
	gvfsfileinfo.c gvfsfileinfo.h \
	$(dbus_built_sources) \
	$(NULL)

# needed by cygwin (see bug #564003)
libgvfscommon_la_LDFLAGS = -no-undefined -avoid-version

libgvfscommon_la_LIBADD =	\
	$(GLIB_LIBS)		\
	$(BLURAY_LIBS)

libgvfscommon_monitor_la_SOURCES = 	\
	gvfsmountinfo.h gvfsmountinfo.c \
	$(NULL)

libgvfscommon_monitor_la_CFLAGS =	\
	$(GLIB_LIBS)			\
	$(BLURAY_CFLAGS)

libgvfscommon_monitor_la_LIBADD =	\
	libgvfscommon.la 		\
	$(BLURAY_LIBS) 			\
	$(GLIB_LIBS)

if HAVE_AVAHI
noinst_LTLIBRARIES += libgvfscommon-dnssd.la

libgvfscommon_dnssd_la_SOURCES = 	\
	gvfsdnssdutils.c gvfsdnssdutils.h \
	gvfsdnssdresolver.c gvfsdnssdresolver.h \
	$(NULL)

libgvfscommon_dnssd_la_CFLAGS =	\
	$(AVAHI_CFLAGS)

libgvfscommon_dnssd_la_LIBADD =	\
	libgvfscommon.la \
	$(AVAHI_LIBS) \
	$(GLIB_LIBS)
endif


EXTRA_DIST = org.gtk.vfs.xml

CLEANFILES = $(dbus_built_sources)
