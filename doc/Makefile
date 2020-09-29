# Makefile for mrouted, a multicast router, and its auxiliary   -*-Makefile-*-
# programs: map-mbone, mrinfo and mtrace.
#

# VERSION       ?= $(shell git tag -l | tail -1)
VERSION      ?= 3.9.8
NAME          = mrouted
CONFIG        = $(NAME).conf
EXECS         = mrouted map-mbone mrinfo mtrace
PKG           = $(NAME)-$(VERSION)
ARCHIVE       = $(PKG).tar.bz2

ROOTDIR      ?= $(dir $(shell pwd))
RM           ?= rm -f
CC           ?= $(CROSS)gcc

prefix       ?= /usr/local
sysconfdir   ?= /etc
datadir       = $(prefix)/share/doc/mrouted
mandir        = $(prefix)/share/man/man8

# This magic trick looks like a comment, but works on BSD PMake
#include <config.mk>
include config.mk

IGMP_SRCS     = igmp.c inet.c kern.c
IGMP_OBJS     = igmp.o inet.o kern.o

ROUTER_OBJS   = config.o cfparse.o main.o route.o vif.o prune.o callout.o \
		icmp.o ipip.o vers.o $(RSRR_OBJS) $(EXTRA_OBJS)
ROUTER_SRCS   = $(ROUTER_OBJS:.o=.c)
MAPPER_OBJS   = mapper.o $(EXTRA_OBJS)
MRINFO_OBJS   = mrinfo.o $(EXTRA_OBJS)
MTRACE_OBJS   = mtrace.o $(EXTRA_OBJS)
#MSTAT_OBJS    = mstat.o $(EXTRA_OBJS)

## Common
CFLAGS       += $(MCAST_INCLUDE) $(INCLUDES) $(DEFS) $(USERCOMPILE)
CFLAGS       += -O2 -W -Wall -Wextra
#CFLAGS       += -O -g
LDLIBS        = $(EXTRA_LIBS)
LDFLAGS      += -Wl,-Map,$@.map
OBJS          = $(IGMP_OBJS) $(ROUTER_OBJS) $(MAPPER_OBJS) $(MRINFO_OBJS) \
		$(MTRACE_OBJS) $(MSTAT_OBJS)
SRCS          = $(OBJS:.o=.c)
MANS          = $(addsuffix .8,$(EXECS))
DISTFILES     = README.md AUTHORS LICENSE ChangeLog.md

LINT          = splint
LINTFLAGS     = $(MCAST_INCLUDE) $(filter-out -W -Wall -Werror, $(CFLAGS)) -posix-lib -weak -skipposixheaders

all: $(EXECS) $(MSTAT)

.y.c:
	@printf "  YACC    $@\n"
	@$(YACC) $<
	-@mv y.tab.c $@ || mv $(<:.y=.tab.c) $@

.c.o:
	@printf "  CC      $@\n"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: $(EXECS)
	@install -d $(DESTDIR)$(prefix)/sbin
	@install -d $(DESTDIR)$(sysconfdir)
	@install -d $(DESTDIR)$(datadir)
	@install -d $(DESTDIR)$(mandir)
	@for file in $(EXECS); do \
		install -m 0755 $$file $(DESTDIR)$(prefix)/sbin/$$file; \
	done
	@install -b -m 0644 $(CONFIG) $(DESTDIR)$(sysconfdir)/$(CONFIG)
	@for file in $(DISTFILES); do \
		install -m 0644 $$file $(DESTDIR)$(datadir)/$$file; \
	done
	@for file in $(MANS); do \
		install -m 0644 $$file $(DESTDIR)$(mandir)/$$file; \
	done

uninstall:
	-@for file in $(EXECS); do \
		$(RM) $(DESTDIR)$(prefix)/sbin/$$file; \
	done
	-@$(RM) $(DESTDIR)$(sysconfdir)/$(CONFIG)
	-@$(RM) -r $(DESTDIR)$(datadir)
	-@for file in $(MANS); do \
		$(RM) $(DESTDIR)$(mandir)/$$file; \
	done

mrouted: $(IGMP_OBJS) $(ROUTER_OBJS) $(CMULIBS)
	@printf "  LINK    $@\n"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IGMP_OBJS) $(ROUTER_OBJS) $(LDLIBS)

vers.c: Makefile
	@echo $(VERSION) | sed -e 's/.*/char todaysversion[]="&";/' > vers.c

map-mbone: $(IGMP_OBJS) $(MAPPER_OBJS)
	@printf "  LINK    $@\n"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IGMP_OBJS) $(MAPPER_OBJS) $(LDLIBS)

mrinfo: $(IGMP_OBJS) $(MRINFO_OBJS)
	@printf "  LINK    $@\n"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IGMP_OBJS) $(MRINFO_OBJS) $(LDLIBS)

mtrace: $(IGMP_OBJS) $(MTRACE_OBJS)
	@printf "  LINK    $@\n"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IGMP_OBJS) $(MTRACE_OBJS) $(LDLIBS)

mstat: $(MSTAT_OBJS) $(CMULIBS)
	@printf "  LINK    $@\n"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSTAT_OBJS) $(LDLIBS)

# Runs Clang scan-build on the whole tree
check: clean
	@scan-build $(MAKE) all

clean:
	-@$(RM) $(OBJS) $(EXECS)

distclean:
	-@$(RM) $(OBJS) core $(EXECS) vers.c cfparse.c *.o *.map .*.d *.out tags TAGS

dist:
	@echo "Building bzip2 tarball of $(PKG) in parent dir..."
	git archive --format=tar --prefix=$(PKG)/ $(VERSION) | bzip2 >../$(ARCHIVE)
	@(cd ..; md5sum $(ARCHIVE) | tee $(ARCHIVE).md5)

build-deb:
	gbp buildpackage --git-ignore-new --git-ignore-branch --git-no-create-orig --git-upstream-branch=master

lint: 
	@$(LINT) $(LINTFLAGS) $(SRCS)

tags: $(IGMP_SRCS) $(ROUTER_SRCS)
	@ctags $(IGMP_SRCS) $(ROUTER_SRCS)

cflow:
	@cflow $(MCAST_INCLUDE) $(IGMP_SRCS) $(ROUTER_SRCS) > cflow.out

cflow2:
	@cflow -ix $(MCAST_INCLUDE) $(IGMP_SRCS) $(ROUTER_SRCS) > cflow2.out

rcflow:
	@cflow -r $(MCAST_INCLUDE) $(IGMP_SRCS) $(ROUTER_SRCS) > rcflow.out

rcflow2:
	@cflow -r -ix $(MCAST_INCLUDE) $(IGMP_SRCS) $(ROUTER_SRCS) > rcflow2.out

TAGS:
	@etags $(SRCS)

