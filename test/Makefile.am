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

%.monitor:
	$(AM_V_GEN) ln -s $(top_srcdir)/monitor/$(*F)/$(@F) $@

CONFIG_FILES=session.conf gvfs-daemon.service gvfs-metadata.service

if USE_AFC
org.gtk.Private.AfcVolumeMonitor.service: $(top_srcdir)/monitor/afc/org.gtk.Private.AfcVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/afc|" $< > $@
CONFIG_FILES+=org.gtk.Private.AfcVolumeMonitor.service afc.monitor
endif

if USE_GDU
org.gtk.Private.GduVolumeMonitor.service: $(top_srcdir)/monitor/gdu/org.gtk.Private.GduVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/gdu|" $< > $@
CONFIG_FILES+=org.gtk.Private.GduVolumeMonitor.service gdu.monitor
endif

if USE_GPHOTO2
org.gtk.Private.GPhoto2VolumeMonitor.service: $(top_srcdir)/monitor/gphoto2/org.gtk.Private.GPhoto2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/gphoto2|" $< > $@
CONFIG_FILES+=org.gtk.Private.GPhoto2VolumeMonitor.service gphoto2.monitor
endif

if USE_HAL
org.gtk.Private.HalVolumeMonitor.service: $(top_srcdir)/monitor/hal/org.gtk.Private.HalVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/hal|" $< > $@
CONFIG_FILES+=org.gtk.Private.HalVolumeMonitor.service hal.monitor
endif

if USE_UDISKS2
org.gtk.Private.UDisks2VolumeMonitor.service: $(top_srcdir)/monitor/udisks2/org.gtk.Private.UDisks2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/udisks2|" $< > $@
CONFIG_FILES+=org.gtk.Private.UDisks2VolumeMonitor.service udisks2.monitor
endif

noinst_DATA= $(CONFIG_FILES)

# run tests against build tree
check: $(CONFIG_FILES) gvfs-test
	$(srcdir)/run-in-tree.sh $(srcdir)/gvfs-test $(TEST_NAMES)

# run tests against the installed system packages
# when running as root, use gvfs-testbed to enable all tests
installcheck-local: gvfs-test
	if [ `id -u` = 0 ]; then \
	    $(srcdir)/gvfs-testbed $(srcdir)/gvfs-test $(TEST_NAMES); \
	else \
	    $(srcdir)/gvfs-test $(TEST_NAMES); \
	fi

CLEANFILES=$(CONFIG_FILES)

EXTRA_DIST = \
	benchmark-common.c		\
	session.conf.in 		\
	gvfs-test			\
	gvfs-testbed			\
	run-in-tree.sh			\
	test_polkitd.py			\
	files/ssh_host_rsa_key files	\
	files/ssh_host_rsa_key.pub	\
	files/testcert.pem		\
	files/powershot.ioctl		\
	files/powershot.umockdev	\
	$(NULL)
