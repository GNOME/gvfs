include $(top_srcdir)/glib-tap.mk

NULL =

AM_CFLAGS =                       \
	-I$(top_srcdir)           \
	-I$(top_builddir)         \
	$(GLIB_CFLAGS)

AM_LDFLAGS =                           \
	$(GLIB_LIBS)

noinst_PROGRAMS += \
	test-query-info-stream    \
	benchmark-gvfs-small-files    \
	benchmark-gvfs-big-files      \
	benchmark-posix-small-files   \
	benchmark-posix-big-files     \
	$(NULL)

session.conf: session.conf.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@testdir\@|.|" $< > $@

org.gtk.vfs.Daemon.service: $(top_srcdir)/daemon/org.gtk.vfs.Daemon.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/daemon|" $< > $@

org.gtk.vfs.Metadata.service: $(top_srcdir)/metadata/org.gtk.vfs.Metadata.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/metadata|" $< > $@

%.monitor:
	$(AM_V_GEN) ln -s $(top_srcdir)/monitor/$(*F)/$(@F) $@

CONFIG_FILES=session.conf org.gtk.vfs.Daemon.service org.gtk.vfs.Metadata.service
CONFIG_FILES_INSTALLED=session.conf $(top_builddir)/daemon/org.gtk.vfs.Daemon.service $(top_builddir)/metadata/org.gtk.vfs.Metadata.service

if USE_AFC
org.gtk.vfs.AfcVolumeMonitor.service: $(top_srcdir)/monitor/afc/org.gtk.vfs.AfcVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/afc|" $< > $@
CONFIG_FILES+=org.gtk.vfs.AfcVolumeMonitor.service afc.monitor
CONFIG_FILES_INSTALLED+=$(top_builddir)/monitor/afc/org.gtk.vfs.AfcVolumeMonitor.service afc.monitor
endif

if USE_GDU
org.gtk.vfs.GduVolumeMonitor.service: $(top_srcdir)/monitor/gdu/org.gtk.vfs.GduVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/gdu|" $< > $@
CONFIG_FILES+=org.gtk.vfs.GduVolumeMonitor.service gdu.monitor
CONFIG_FILES_INSTALLED+=$(top_builddir)/monitor/afc/org.gtk.vfs.GduVolumeMonitor.service gdu.monitor
endif

if USE_GPHOTO2
org.gtk.vfs.GPhoto2VolumeMonitor.service: $(top_srcdir)/monitor/gphoto2/org.gtk.vfs.GPhoto2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/gphoto2|" $< > $@
CONFIG_FILES+=org.gtk.vfs.GPhoto2VolumeMonitor.service gphoto2.monitor
CONFIG_FILES_INSTALLED+=$(top_builddir)/monitor/gphoto2/org.gtk.vfs.GPhoto2VolumeMonitor.service gphoto2.monitor
endif

if USE_HAL
org.gtk.vfs.HalVolumeMonitor.service: $(top_srcdir)/monitor/hal/org.gtk.vfs.HalVolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/hal|" $< > $@
CONFIG_FILES+=org.gtk.vfs.HalVolumeMonitor.service hal.monitor
CONFIG_FILES_INSTALLED+=$(top_builddir)/monitor/hal/org.gtk.vfs.HalVolumeMonitor.service hal.monitor
endif

if USE_UDISKS2
org.gtk.vfs.UDisks2VolumeMonitor.service: $(top_srcdir)/monitor/udisks2/org.gtk.vfs.UDisks2VolumeMonitor.service.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/monitor/udisks2|" $< > $@
CONFIG_FILES+=org.gtk.vfs.UDisks2VolumeMonitor.service udisks2.monitor
CONFIG_FILES_INSTALLED+=$(top_builddir)/monitor/udisks2/org.gtk.vfs.UDisks2VolumeMonitor.service udisks2.monitor
endif

noinst_DATA += $(CONFIG_FILES)

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

CLEANFILES += $(CONFIG_FILES)

installed_test_DATA = $(CONFIG_FILES_INSTALLED)
nobase_installed_test_DATA += \
	files/source-gphoto/IMG_0001.jpg \
	files/source-gphoto/IMG_0002.jpg \
	files/bogus-cd.iso.bz2 \
	files/joliet.iso.bz2 \
	files/powershot.ioctl \
	files/powershot.umockdev \
	files/ssh_host_rsa_key \
	files/ssh_host_rsa_key.pub \
	files/testcert.pem \
	files/vfat.img.bz2 \
	$(NULL)

test_extra_scripts = \
	gvfs-test \
	$(NULL)

test_scripts = gvfs-all-tests

gvfs-all-tests.test: gvfs-test
	$(AM_V_GEN) (echo '[Test]' > $@.tmp; \
	echo 'Type=session' >> $@.tmp; \
	echo 'Exec=$(installed_testdir)/gvfs-test' >> $@.tmp; \
	echo 'Output=TAP' >> $@.tmp; \
	mv $@.tmp $@)

.PHONY: $(test_scripts)

EXTRA_DIST += \
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
