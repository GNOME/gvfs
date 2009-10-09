NULL =

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

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/common \
	$(LIBXML_CFLAGS) $(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	$(UDEV_CFLAGS) \
	-DGVFS_LOCALEDIR=\""$(localedir)"\"     \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

libmetadata_la_SOURCES = 		\
	metatree.c metatree.h		\
	metabuilder.c metabuilder.h 	\
	crc32.c crc32.h			\
	$(NULL)

libmetadata_la_LIBADD = $(GLIB_LIBS) $(UDEV_LIBS)

meta_ls_LDADD = libmetadata.la
meta_ls_SOURCES = meta-ls.c

meta_set_LDADD = libmetadata.la $(DBUS_LIBS) ../common/libgvfscommon.la
meta_set_SOURCES = meta-set.c

meta_get_LDADD = libmetadata.la
meta_get_SOURCES = meta-get.c

meta_get_tree_LDADD = libmetadata.la
meta_get_tree_SOURCES = meta-get-tree.c

convert_nautilus_metadata_LDADD = libmetadata.la $(LIBXML_LIBS)
convert_nautilus_metadata_SOURCES = metadata-nautilus.c

gvfsd_metadata_LDADD = libmetadata.la $(DBUS_LIBS) ../common/libgvfscommon.la
gvfsd_metadata_SOURCES = meta-daemon.c

# D-BUS service file
%.service: %.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = gvfs-metadata.service.in
service_DATA = gvfs-metadata.service

EXTRA_DIST = gvfs-metadata.service.in

clean-local:
	rm -f gvfs-metadata.service
