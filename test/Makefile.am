NULL =

AM_CFLAGS =                       \
	-I$(top_srcdir)           \
	-I$(top_builddir)         \
	$(GLIB_CFLAGS)

AM_LDFLAGS =                           \
	$(GLIB_LIBS)

noinst_PROGRAMS = \
	test-query-info-stream    \
	benchmark-gvfs-small-files    \
	benchmark-gvfs-big-files      \
	benchmark-posix-small-files   \
	benchmark-posix-big-files     \
	$(NULL)

session.conf: session.conf.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@testdir\@|$(abs_builddir)|" $< > $@

gvfs-daemon.service: $(top_srcdir)/daemon/gvfs-daemon.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/daemon|" $< > $@

gvfs-metadata.service: $(top_srcdir)/metadata/gvfs-metadata.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/metadata|" $< > $@

noinst_DATA= session.conf gvfs-daemon.service gvfs-metadata.service

if USE_AFC
org.gtk.Private.AfcVolumeMonitor.service: $(top_srcdir)/monitor/afc/org.gtk.Private.AfcVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor//afc|" $< > $@
noinst_DATA+=org.gtk.Private.AfcVolumeMonitor.service
endif

if USE_GDU
org.gtk.Private.GduVolumeMonitor.service: $(top_srcdir)/monitor/gdu/org.gtk.Private.GduVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor//gdu|" $< > $@
noinst_DATA+=org.gtk.Private.GduVolumeMonitor.service
endif

if USE_GPHOTO2
org.gtk.Private.GPhoto2VolumeMonitor.service: $(top_srcdir)/monitor/gphoto2/org.gtk.Private.GPhoto2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor//gphoto2|" $< > $@
noinst_DATA+=org.gtk.Private.GPhoto2VolumeMonitor.service
endif

if USE_HAL
org.gtk.Private.HalVolumeMonitor.service: $(top_srcdir)/monitor/hal/org.gtk.Private.HalVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor//hal|" $< > $@
noinst_DATA+=org.gtk.Private.HalVolumeMonitor.service
endif

if USE_UDISKS2
org.gtk.Private.UDisks2VolumeMonitor.service: $(top_srcdir)/monitor/udisks2/org.gtk.Private.UDisks2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor//udisks2|" $< > $@
noinst_DATA+=org.gtk.Private.UDisks2VolumeMonitor.service
endif

test: session.conf gvfs-daemon.service gvfs-metadata.service
	./run-in-tree.sh ./gvfs-test

EXTRA_DIST = benchmark-common.c session.conf.in gvfs-test
