# config.mk                                                     -*-Makefile-*-
# This is the mrouted build configuration file.  See the below description for
# details on each build option.

# -D_GNU_SOURCE : Use GNU extensions, where possible
# -D_BSD_SOURCE : Use functions derived from 4.3 BSD Unix rather than POSIX.1
DEFS = -D_BSD_SOURCE -D_GNU_SOURCE

##
# Compilation flags for different platforms. 
# Uncomment only one of them. Default: Linux

# If the multicast header files are not in the standard place on your system,
# define MCAST_INCLUDE to be an appropriate `-I' options for the C compiler.
#MCAST_INCLUDE=	-I/sys

## FreeBSD	-D__FreeBSD__ is defined by the OS
## FreeBSD-3.x, FreeBSD-4.x, FreeBSD-8.x
#INCLUDES     =
#DEFS        += -DHAVE_STRTONUM -DHAVE_STRLCPY
#EXTRA_OBJS   = pidfile.o
#EXTRA_LIBS   =
## FreeBSD-2.x
#INCLUDES     =
#DEFS        +=
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o

## NetBSD	-DNetBSD is defined by the OS
#INCLUDES     =
#DEFS        += -DHAVE_STRLCPY -DHAVE_PIDFILE
#EXTRA_OBJS   = strtonum.o
#EXTRA_LIBS   = -lutil

## OpenBSD	-DOpenBSD is defined by the OS
#INCLUDES     =
#DEFS        += -DHAVE_STRTONUM -DHAVE_STRLCPY -DHAVE_PIDFILE
#EXTRA_OBJS   =
#EXTRA_LIBS   = -lutil

## BSDI		-D__bsdi__ is defined by the OS
#INCLUDES     =
#DEFS        +=
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o

## SunOS, OSF1, gcc
#INCLUDES     =
#DEFS        += -DSunOS=43
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o

## SunOS, OSF1, cc
#INCLUDES     =
#DEFS        += -DSunOS=43
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o

## IRIX
#INCLUDES     =
#DEFS        += -D_BSD_SIGNALS -DIRIX
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o

## Solaris 2.5, gcc
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=55
## Solaris 2.5, cc
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=55
## Solaris 2.6
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=56
## Solaris 2.x
#EXTRA_OBJS   = strlcpy.o pidfile.o strtonum.o
#EXTRA_LIBS   = -L/usr/ucblib -lucb -L/usr/lib -lsocket -lnsl

## Linux	-D__linux__ is defined by the OS
# For uClibc based Linux systems, add -DHAVE_STRLCPY to DEFS
INCLUDES      =
DEFS         += -DIOCTL_OK_ON_RAW_SOCKET
EXTRA_OBJS    = strlcpy.o pidfile.o strtonum.o
