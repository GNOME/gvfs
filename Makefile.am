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

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = gio.pc
