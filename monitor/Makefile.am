DIST_SUBDIRS = proxy gdu gphoto2 afc udisks2 mtp goa
SUBDIRS = proxy

if USE_GDU
SUBDIRS += gdu
endif

if USE_UDISKS2
SUBDIRS += udisks2
endif

if USE_GPHOTO2
SUBDIRS += gphoto2
endif

if USE_AFC
SUBDIRS += afc
endif

if USE_LIBMTP
SUBDIRS += mtp
endif

if USE_GOA
SUBDIRS += goa
endif

EXTRA_DIST = meson.build
