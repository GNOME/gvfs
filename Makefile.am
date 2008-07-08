NULL =

SUBDIRS = \
	common \
	client \
	daemon \
	monitor \
	gconf \
	po   \
	programs \
	test \
	$(NULL)

if USE_HAL
SUBDIRS += hal
endif

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

EXTRA_DIST = \
	MAINTAINERS		\
	intltool-extract.in	\
	intltool-merge.in	\
	intltool-update.in	\
	$(NULL)

DISTCLEANFILES = \
	intltool-extract	\
	intltool-merge		\
	intltool-update		\
	$(NULL)
