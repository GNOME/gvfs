NULL =

SUBDIRS = \
	common \
	metadata \
	client \
	daemon \
	monitor \
	po   \
	programs \
	test \
	$(NULL)

if BUILD_DOCUMENTATION
SUBDIRS += man
endif

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

EXTRA_DIST = \
	ChangeLog.pre-1-2	\
	gvfs.doap		\
	README.commits		\
	MAINTAINERS		\
	m4			\
	gettext/its/polkit.loc	\
	gettext/its/polkit.its	\
	meson.build		\
	meson_options.txt	\
	meson_post_install.py	\
	dbus.service.in		\
	po/meson.build		\
	$(NULL)

distclean-local:
	if test "$(srcdir)" = "."; then :; else \
		rm -f ChangeLog; \
	fi

install-data-hook:
	if test -z "$(DESTDIR)" -a "$(GIO_QUERYMODULES)" != "no" ; then					\
		$(GIO_QUERYMODULES) $(giomodulesdir) ;	\
	fi

uninstall-local:
	$(RM) $(DESTDIR)$(giomodulesdir)/giomodule.cache

ChangeLog:
	$(AM_V_GEN) if test -d "$(srcdir)/.git"; then \
	  (GIT_DIR=$(top_srcdir)/.git ./missing --run git log GVFS_1_2_2.. --stat -M -C --name-status  --date=short --no-color) | fmt --split-only > $@.tmp \
	  && mv -f $@.tmp $@ \
	  || ($(RM) $@.tmp; \
	      echo Failed to generate ChangeLog, your ChangeLog may be outdated >&2; \
	      (test -f $@ || echo git-log is required to generate this file >> $@)); \
	else \
	  test -f $@ || \
	  (echo A git checkout and git-log is required to generate ChangeLog >&2 && \
	  echo A git checkout and git-log is required to generate this file >> $@); \
	fi

.PHONY: ChangeLog
