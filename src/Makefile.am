# For replacement functions in lib/
AUTOMAKE_OPTIONS = subdir-objects

# -D_GNU_SOURCE Use GNU extensions, where possible
# -D_BSD_SOURCE Use functions derived from 4.3 BSD Unix rather than POSIX.1
#               In GLIBC >= v2.20 this is replaced with -D_DEFAULT_SOURCE,
#               but to build on older GLIBC systems we now need both ...
AM_CPPFLAGS      = -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE -I$(top_srcdir)/include
AM_CPPFLAGS     += -DSYSCONFDIR=\"@sysconfdir@\" -DLOCALSTATEDIR=\"@localstatedir@\"
if LINUX
AM_CPPFLAGS     += -DIOCTL_OK_ON_RAW_SOCKET
endif

sbin_PROGRAMS    = mrouted map-mbone mrinfo mtrace
mrouted_SOURCES  = main.c callout.c cfparse.y config.c defs.h dvmrp.h icmp.c \
		   igmp.c igmpv2.h ipip.c inet.c kern.c pathnames.h prune.c  \
	 	   route.c route.h vers.c vif.c vif.h
mrouted_CFLAGS   = -W -Wall -Wextra
mrouted_LDADD    = $(LIBS) $(LIBOBJS)
if RSRR
mrouted_SOURCES += rsrr.c rsrr_var.h rsrr.h
endif

map_mbone_SOURCES = mapper.c igmp.c igmpv2.h inet.c kern.c
map_mbone_LDADD   = $(LIBS) $(LIBOBJS)

mrinfo_SOURCES    = mrinfo.c igmp.c igmpv2.h inet.c kern.c
mrinfo_LDADD      = $(LIBS) $(LIBOBJS)

mtrace_SOURCES    = mtrace.c igmp.c igmpv2.h inet.c kern.c
mtrace_LDADD      = $(LIBS) $(LIBOBJS)

vers.c: Makefile
	@echo $(VERSION) | sed -e 's/.*/char todaysversion[]="&";/' > vers.c

