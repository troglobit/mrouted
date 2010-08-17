# -*-Makefile-*-
#
# Makefile for mrouted, a multicast router, and its auxiliary programs,
# map-mbone, mrinfo and mtrace.
#
# Makefile,v 3.8.4.7 1998/03/01 03:09:11 fenner Exp
#

# VERSION       ?= $(shell git tag -l | tail -1)
VERSION      ?= 3.9.2-rc2
NAME          = mrouted
CONFIG        = $(NAME).conf
EXECS         = mrouted map-mbone mrinfo mtrace
PKG           = $(NAME)-$(VERSION)
ARCHIVE       = $(PKG).tar.bz2

ROOTDIR      ?= $(dir $(shell pwd))
CC           ?= $(CROSS)gcc

prefix       ?= /usr/local
sysconfdir   ?= /etc
datadir       = $(prefix)/share/doc/mrouted
mandir        = $(prefix)/share/man/man8

# Uncomment the following three lines if you want to use RSRR (Routing
# Support for Resource Reservations), currently used by RSVP.
RSRRDEF       = -DRSRR
RSRR_OBJS     = rsrr.o

IGMP_SRCS     = igmp.c inet.c kern.c
IGMP_OBJS     = igmp.o inet.o kern.o
ROUTER_OBJS   = config.o cfparse.o main.o route.o vif.o prune.o callout.o \
		icmp.o ipip.o ${RSRR_OBJS}
ifndef HAVE_STRLCPY
ROUTER_OBJS  += strlcpy.o
endif
ifndef HAVE_STRTONUM
ROUTER_OBJS  += strtonum.o
endif
ifndef HAVE_PIDFILE
ROUTER_OBJS  += pidfile.o
endif
ROUTER_SRCS   = $(ROUTER_OBJS:.o=.c)

MAPPER_OBJS   = mapper.o
ifndef HAVE_STRLCPY
MAPPER_OBJS  += strlcpy.o
endif
ifndef HAVE_STRTONUM
MAPPER_OBJS  += strtonum.o
endif

MRINFO_OBJS   = mrinfo.o
ifndef HAVE_STRTONUM
MRINFO_OBJS  += strtonum.o
endif

MTRACE_OBJS   = mtrace.o
ifndef HAVE_STRTONUM
MTRACE_OBJS  += strtonum.o
endif

#MSTAT_SRCS    = mstat.c 
#MSTAT_OBJS    = mstat.o

include rules.mk
include config.mk
include snmp.mk

## Common
CFLAGS        = ${MCAST_INCLUDE} ${SNMPDEF} ${RSRRDEF} $(INCLUDES) $(DEFS) $(USERCOMPILE)
CFLAGS       += -O2 -W -Wall -Werror
#CFLAGS       += -O -g
LDLIBS        = ${SNMPLIBDIR} ${SNMPLIBS} ${LIB2}
OBJS          = ${IGMP_OBJS} ${ROUTER_OBJS} ${MAPPER_OBJS} ${MRINFO_OBJS} \
		${MTRACE_OBJS} ${MSTAT_OBJS}
SRCS          = $(OBJS:.o=.c)
DEPS          = $(filter-out .cfparse.d, $(addprefix .,$(SRCS:.c=.d)))
MANS          = $(addsuffix .8,$(EXECS))
DISTFILES     = README AUTHORS LICENSE CHANGES

LINT          = splint
LINTFLAGS     = ${MCAST_INCLUDE} $(filter-out -W -Wall -Werror, $(CFLAGS)) -posix-lib -weak -skipposixheaders

all: $(EXECS) ${MSTAT}

install: $(EXECS)
	$(Q)[ -n "$(DESTDIR)" -a ! -d $(DESTDIR) ] || install -d $(DESTDIR)
	$(Q)install -d $(DESTDIR)$(prefix)/sbin
	$(Q)install -d $(DESTDIR)$(sysconfdir)
	$(Q)install -d $(DESTDIR)$(datadir)
	$(Q)install -d $(DESTDIR)$(mandir)
	$(Q)for file in $(EXECS); do \
		install -m 0755 $$file $(DESTDIR)$(prefix)/sbin/$$file; \
	done
	$(Q)install --backup=existing -m 0644 $(CONFIG) $(DESTDIR)$(sysconfdir)/$(CONFIG)
	$(Q)for file in $(DISTFILES); do \
		install -m 0644 $$file $(DESTDIR)$(datadir)/$$file; \
	done
	$(Q)for file in $(MANS); do \
		install -m 0644 $$file $(DESTDIR)$(mandir)/$$file; \
	done

uninstall:
	-$(Q)for file in $(EXECS); do \
		$(RM) $(DESTDIR)$(prefix)/sbin/$$file; \
	done
	-$(Q)$(RM) $(DESTDIR)$(sysconfdir)/$(CONFIG)
	-$(Q)$(RM) -r $(DESTDIR)$(datadir)
	-$(Q)for file in $(MANS); do \
		$(RM) $(DESTDIR)$(mandir)/$$file; \
	done

mrouted: ${IGMP_OBJS} ${ROUTER_OBJS} vers.o ${CMULIBS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${CFLAGS} ${LDFLAGS} -Wl,-Map,$@.map -o $@ $^ $(LDLIBS$(LDLIBS-$(@)))

vers.c: Makefile
	@echo $(VERSION) | sed -e 's/.*/char todaysversion[]="&";/' > vers.c

map-mbone: ${IGMP_OBJS} ${MAPPER_OBJS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${IGMP_OBJS} ${MAPPER_OBJS} ${LIB2}

mrinfo: ${IGMP_OBJS} ${MRINFO_OBJS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${IGMP_OBJS} ${MRINFO_OBJS} ${LIB2}

mtrace: ${IGMP_OBJS} ${MTRACE_OBJS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${IGMP_OBJS} ${MTRACE_OBJS} ${LIB2}

mstat: ${MSTAT_OBJS} snmplib/libsnmp.a
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${MSTAT_OBJS} -Lsnmplib -lsnmp

clean: ${SNMPCLEAN}
	-$(Q)$(RM) $(OBJS) $(EXECS)

distclean:
	-$(Q)$(RM) $(OBJS) core $(EXECS) vers.c cfparse.c tags TAGS *.o *.map .*.d *.out tags TAGS

dist:
	@echo "Building bzip2 tarball of $(PKG) in parent dir..."
	git archive --format=tar --prefix=$(PKG)/ $(VERSION) | bzip2 >../$(ARCHIVE)
	@(cd ..; md5sum $(ARCHIVE))

lint: 
	@$(LINT) $(LINTFLAGS) $(SRCS)

tags: ${IGMP_SRCS} ${ROUTER_SRCS}
	@ctags ${IGMP_SRCS} ${ROUTER_SRCS}

cflow:
	@cflow ${MCAST_INCLUDE} ${IGMP_SRCS} ${ROUTER_SRCS} > cflow.out

cflow2:
	@cflow -ix ${MCAST_INCLUDE} ${IGMP_SRCS} ${ROUTER_SRCS} > cflow2.out

rcflow:
	@cflow -r ${MCAST_INCLUDE} ${IGMP_SRCS} ${ROUTER_SRCS} > rcflow.out

rcflow2:
	@cflow -r -ix ${MCAST_INCLUDE} ${IGMP_SRCS} ${ROUTER_SRCS} > rcflow2.out

TAGS:
	@etags ${SRCS}

snmpclean:
	-(cd snmpd; make clean)
	-(cd snmplib; make clean)

ifneq ($(MAKECMDGOALS),dist)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),distclean)
-include $(DEPS)
endif
endif
endif
