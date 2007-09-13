NULL =

if OS_UNIX
unix_dirs = \
	common \
	client \
	daemon \
	$(NULL)

endif

SUBDIRS = \
	gio \
	$(unix_dirs) \
	po   \
	programs \
	$(NULL)

if USE_FUSE
SUBDIRS += fuse
endif

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = gio-standalone.pc
