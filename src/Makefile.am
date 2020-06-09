# For replacement functions in lib/
AUTOMAKE_OPTIONS = subdir-objects

# -D_GNU_SOURCE Use GNU extensions, where possible
# -D_BSD_SOURCE Use functions derived from 4.3 BSD Unix rather than POSIX.1
#               In GLIBC >= v2.20 this is replaced with -D_DEFAULT_SOURCE,
#               but to build on older GLIBC systems we now need both ...
AM_CPPFLAGS      = -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE -I$(top_srcdir)/include
AM_CPPFLAGS     += -DSYSCONFDIR=\"@sysconfdir@\" -DRUNSTATEDIR=\"@runstatedir@\" -DPRESERVEDIR=\"@localstatedir@/lib\"
if LINUX
AM_CPPFLAGS     += -DIOCTL_OK_ON_RAW_SOCKET
endif
AM_CFLAGS        = -W -Wall -Wextra -Wno-unused -Wno-unused-parameter

sbin_PROGRAMS    = mrouted mroutectl map-mbone mrinfo mtrace
mrouted_SOURCES  = main.c common.c cfparse.y config.c 	\
		   defs.h dvmrp.h			\
		   igmp.c igmpv2.h igmpv3.h 		\
		   icmp.c ipc.c ipip.c inet.c		\
		   kern.c log.c 			\
		   pathnames.h queue.h 			\
		   prune.c prune.h 			\
		   route.c route.h 			\
		   timer.c 				\
		   vif.c vif.h
mrouted_LDADD    = $(LIBS) $(LIBOBJS)
if RSRR
mrouted_SOURCES += rsrr.c rsrr_var.h rsrr.h
endif

mroutectl_SOURCES = mroutectl.c common.c defs.h
mroutectl_LDADD   = $(LIBS) $(LIBOBJS)

map_mbone_SOURCES = mapper.c defs.h igmp.c igmpv2.h inet.c kern.c
map_mbone_LDADD   = $(LIBS) $(LIBOBJS)

mrinfo_SOURCES    = mrinfo.c defs.h igmp.c igmpv2.h inet.c kern.c
mrinfo_LDADD      = $(LIBS) $(LIBOBJS)

mtrace_SOURCES    = mtrace.c defs.h igmp.c igmpv2.h inet.c kern.c
mtrace_LDADD      = $(LIBS) $(LIBOBJS)

