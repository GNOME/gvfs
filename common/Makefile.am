NULL =

lib_LTLIBRARIES=libgvfscommon.la

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/daemon \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) $(BLURAY_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" \
	-DDBUS_API_SUBJECT_TO_CHANGE

dbus_built_sources = gvfsdbus.c gvfsdbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources) : Makefile.am dbus-interfaces.xml
	$(AM_V_GEN) gdbus-codegen			\
		--interface-prefix org.gtk.vfs.		\
		--c-namespace GVfsDBus			\
		--generate-c-code gvfsdbus		\
		$(srcdir)/dbus-interfaces.xml		\
		$(NULL)

libgvfscommon_la_SOURCES = 	\
	gsysutils.c gsysutils.h \
	gvfsdbusutils.c gvfsdbusutils.h \
	gmountspec.c gmountspec.h \
	gmountoperationdbus.c gmountoperationdbus.h \
	gmountsource.c gmountsource.h \
	gmounttracker.c gmounttracker.h \
	gvfsdaemonprotocol.c gvfsdaemonprotocol.h \
	gvfsicon.h gvfsicon.c \
	gvfsmountinfo.h gvfsmountinfo.c \
	gvfsfileinfo.c gvfsfileinfo.h \
	$(dbus_built_sources) \
	$(NULL)

# needed by cygwin (see bug #564003)
libgvfscommon_la_LDFLAGS = -no-undefined

libgvfscommon_la_LIBADD =	\
	$(DBUS_LIBS) 		\
	$(GLIB_LIBS)		\
	$(BLURAY_LIBS)

if HAVE_AVAHI
lib_LTLIBRARIES += libgvfscommon-dnssd.la

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


EXTRA_DIST = dbus-interfaces.xml

CLEANFILES = $(dbus_built_sources)
