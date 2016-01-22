NULL =

dbus_built_sources = metadata-dbus.c metadata-dbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources) : Makefile.am dbus-interface.xml
	$(AM_V_GEN) gdbus-codegen				\
		--interface-prefix org.gtk.vfs.			\
		--c-namespace GVfs				\
		--generate-c-code metadata-dbus			\
		$(srcdir)/dbus-interface.xml			\
		$(NULL)

noinst_LTLIBRARIES=libmetadata.la

APPS = 	\
	meta-ls		\
	meta-get	\
	meta-set	\
	meta-get-tree	\
	$(NULL)

if HAVE_LIBXML
APPS += convert-nautilus-metadata
endif

noinst_PROGRAMS = $(APPS)

libexec_PROGRAMS =\
	gvfsd-metadata	\
	$(NULL)

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/common \
	$(LIBXML_CFLAGS) $(GLIB_CFLAGS) \
	-DGVFS_LOCALEDIR=\""$(localedir)"\"     \
	-DG_LOG_DOMAIN=\"GVFS\" \
	-DDBUS_API_SUBJECT_TO_CHANGE

libmetadata_la_SOURCES = 		\
	metatree.c metatree.h		\
	metabuilder.c metabuilder.h 	\
	crc32.c crc32.h			\
	$(dbus_built_sources)		\
	$(NULL)

libmetadata_la_LIBADD = $(GLIB_LIBS)

meta_ls_LDADD = libmetadata.la ../common/libgvfscommon.la
meta_ls_SOURCES = meta-ls.c

meta_set_LDADD = libmetadata.la ../common/libgvfscommon.la
meta_set_SOURCES = meta-set.c

meta_get_LDADD = libmetadata.la ../common/libgvfscommon.la
meta_get_SOURCES = meta-get.c

meta_get_tree_LDADD = libmetadata.la ../common/libgvfscommon.la
meta_get_tree_SOURCES = meta-get-tree.c

convert_nautilus_metadata_LDADD = libmetadata.la $(LIBXML_LIBS)
convert_nautilus_metadata_SOURCES = metadata-nautilus.c

gvfsd_metadata_LDADD = libmetadata.la ../common/libgvfscommon.la $(UDEV_LIBS)
gvfsd_metadata_SOURCES = meta-daemon.c
gvfsd_metadata_CFLAFGS = $(UDEV_CFLAGS)

# D-BUS service file
%.service: %.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = org.gtk.vfs.Metadata.service.in
service_DATA = org.gtk.vfs.Metadata.service

systemd_user_in_files = gvfs-metadata.service.in
if USE_SYSTEMD_USER_UNITS
systemd_user_DATA = gvfs-metadata.service
endif

EXTRA_DIST =				\
	$(service_in_files)		\
	$(systemd_user_in_files)	\
	dbus-interface.xml		\
	$(NULL)

CLEANFILES =				\
	$(dbus_built_sources)		\
	$(service_DATA)			\
	$(systemd_user_DATA)		\
	$(NULL)
