doc_DATA         = README.md ChangeLog.md LICENSE mrouted.conf
EXTRA_DIST       = README.md ChangeLog.md LICENSE mrouted.conf
DISTCLEANFILES   = *~ DEADJOE semantic.cache *.gdb *.elf core core.* *.d
SUBDIRS          = src man
EXTRADEBS        = mrinfo*_$(VERSION)* mtrace*_$(VERSION)* map-mbone*_$(VERSION)*

if ENABLE_TEST
SUBDIRS         += test
endif

if SYSTEMD
systemd_DATA     = mrouted.service
endif

## Generate .deb package
package:
	@debuild -uc -us -B --lintian-opts --profile debian -i -I --show-overrides

## Check if tagged in git
release-hook:
	@if [ "x`git tag -l $(PACKAGE_VERSION)|grep $(PACKAGE_VERSION)`" = "x" ]; then	\
		echo;									\
		printf "\e[1m\e[41mCannot find release tag $(PACKAGE_VERSION)\e[0m\n";	\
		printf "\e[1m\e[5mDo release anyway?\e[0m "; read yorn;			\
		if [ "$$yorn" != "y" -a "$$yorn" != "Y" ]; then				\
			printf "OK, aborting release.\n";				\
			exit 1;								\
		fi;									\
		echo;									\
	else										\
		echo;									\
		printf "\e[1m\e[42mFound GIT release tag $(PACKAGE_VERSION)\e[0m\n";	\
		printf "\e[1m\e[44m>>Remember to push tags!\e[0m\n";			\
		echo;									\
	fi

## Target to run when building a release
release: release-hook distcheck
	@for file in $(DIST_ARCHIVES); do		\
		md5sum    $$file > ../$$file.md5;	\
		sha256sum $$file > ../$$file.sha256;	\
	done
	@mv $(DIST_ARCHIVES) ../
	@echo
	@echo "Resulting release files ========================================================================="
	@for file in $(DIST_ARCHIVES); do						\
		printf "%-32s Distribution tarball\n" $$file;				\
		printf "%-32s " $$file.md5; cat ../$$file.md5 | cut -f1 -d' ';		\
		printf "%-32s " $$file.sha256; cat ../$$file.sha256 | cut -f1 -d' ';	\
	done

DISTCHECK_CONFIGURE_FLAGS = --with-systemd=$$dc_install_base/$(systemd)
