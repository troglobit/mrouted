# -*-Makefile-*-
#
# Makefile for mrouted, a multicast router, and its auxiliary programs,
# map-mbone and mrinfo.
#
# Makefile,v 3.8.4.7 1998/03/01 03:09:11 fenner Exp
#

# VERSION       ?= $(shell git tag -l | tail -1)
VERSION      ?= 3.9.0
NAME          = mrouted
CONFIG        = $(NAME).conf
EXECS         = mrouted map-mbone mrinfo
PKG           = $(NAME)-$(VERSION)
ARCHIVE       = $(PKG).tar.bz2

ROOTDIR      ?= $(dir $(shell pwd))
CC            = $(CROSS)gcc

prefix       ?= /usr/local
sysconfdir   ?= /etc
datadir       = $(prefix)/share/doc/pimd
mandir        = $(prefix)/share/man/man8

# If the multicast header files are not in the standard place on your system,
# define MCAST_INCLUDE to be an appropriate `-I' options for the C compiler.
#MCAST_INCLUDE=	-I/sys

# Uncomment the following three lines if you want to use RSRR (Routing
# Support for Resource Reservations), currently used by RSVP.
RSRRDEF       = -DRSRR
RSRR_OBJS     = rsrr.o

## Common
CFLAGS        = -O ${MCAST_INCLUDE} ${SNMPDEF} ${RSRRDEF}

## SunOS, OSF1, FreeBSD, IRIX
#CFLAGS        += 

## Solaris 2.x
#CFLAGS        += -DSYSV -DSUNOS5
#LIB2          = -lsocket -lnsl

## GNU/Linux
CFLAGS       += -D__BSD_SOURCE -D_GNU_SOURCE -DIOCTL_OK_ON_RAW_SOCKET
CFLAGS       += -Iinclude/linux
CFLAGS       += -W -Wall -Wextra
CFLAGS       += $(USERCOMPILE)
LIBS          = ${SNMPLIBDIR} ${SNMPLIBS} ${LIB2}
LINTFLAGS     = ${MCAST_INCLUDE}
IGMP_SRCS     = igmp.c inet.c kern.c
IGMP_OBJS     = igmp.o inet.o kern.o
ROUTER_OBJS   = config.o cfparse.o main.o route.o vif.o prune.o callout.o \
		icmp.o ipip.o ${RSRR_OBJS}
ifndef HAVE_STRTONUM
ROUTER_OBJS  += strtonum.o
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

#MSTAT_SRCS    = mstat.c 
#MSTAT_OBJS    = mstat.o

HDRS          = defs.h dvmrp.h route.h vif.h prune.h igmpv2.h pathnames.h \
		rsrr.h rsrr_var.h
OBJS          = ${IGMP_OBJS} ${ROUTER_OBJS} ${MAPPER_OBJS} ${MRINFO_OBJS} \
		${MSTAT_OBJS}

SRCS          = $(OBJS:.o=.c)
DEPS          = $(filter-out .cfparse.d, $(addprefix .,$(SRCS:.c=.d)))
MANS          = $(addsuffix .8,$(EXECS))

DISTFILES     = README AUTHORS LICENSE $(CONFIG)

include rules.mk
include snmp.mk

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
	$(Q)${CC} ${LDFLAGS} -o $@ ${CFLAGS} ${IGMP_OBJS} ${ROUTER_OBJS} vers.o ${LIBS}

vers.c: Makefile
	@echo $(VERSION) | sed -e 's/.*/char todaysversion[]="&";/' > vers.c

map-mbone: ${IGMP_OBJS} ${MAPPER_OBJS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${LDFLAGS} -o $@ ${CFLAGS} ${IGMP_OBJS} ${MAPPER_OBJS} ${LIB2}

mrinfo: ${IGMP_OBJS} ${MRINFO_OBJS}
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${LDFLAGS} -o $@ ${CFLAGS} ${IGMP_OBJS} ${MRINFO_OBJS} ${LIB2}

mstat: ${MSTAT_OBJS} snmplib/libsnmp.a
ifdef Q
	@printf "  LINK    $(subst $(ROOTDIR),,$(shell pwd))/$@\n"
endif
	$(Q)${CC} ${LDFLAGS} -o $@ ${CFLAGS} ${MSTAT_OBJS} -Lsnmplib -lsnmp

clean: ${SNMPCLEAN}
	-$(Q)$(RM) $(OBJS) $(EXECS)

distclean:
	-$(Q)$(RM) $(OBJS) core $(EXECS) vers.c cfparse.c tags TAGS *.o *.map .*.d *.out tags TAGS

dist:
	@echo "Building bzip2 tarball of $(PKG) in parent dir..."
	git archive --format=tar --prefix=$(PKG)/ $(VERSION) | bzip2 >../$(ARCHIVE)
	@(cd ..; md5sum $(ARCHIVE))

lint: 
	@lint ${LINTFLAGS} ${SRCS}

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
