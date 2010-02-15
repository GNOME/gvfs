NULL =

SUBDIRS = \
	common \
	metadata \
	client \
	daemon \
	monitor \
	gconf \
	po   \
	programs \
	test \
	$(NULL)

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

EXTRA_DIST = \
	ChangeLog.pre-1-2	\
	gvfs.doap		\
	README.commits		\
	MAINTAINERS		\
	$(NULL)

DISTCLEANFILES = \
	intltool-extract	\
	intltool-merge		\
	intltool-update		\
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
	$(RM) $(DESTDIR)$(GIO_MODULE_DIR)/giomodules.cache

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
