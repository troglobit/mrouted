doc_DATA         = README.md ChangeLog.md LICENSE mrouted.conf
EXTRA_DIST       = README.md ChangeLog.md LICENSE mrouted.conf
DISTCLEANFILES   = *~ DEADJOE semantic.cache *.gdb *.elf core core.* *.d
SUBDIRS          = src man

if ENABLE_TEST
SUBDIRS         += test
endif

if SYSTEMD
systemd_DATA     = mrouted.service
endif

package:
	@debuild -uc -us -B --lintian-opts --profile debian -i -I --show-overrides

DISTCHECK_CONFIGURE_FLAGS = --with-systemd=$$dc_install_base/$(systemd)
