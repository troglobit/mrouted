Simple Multicast Routing for UNIX
=================================
[![License Badge][]][License] [![Travis Status][]][Travis] [![Coverity Status][]][Coverity Scan]

<img align="right" src="doc/dvmrp-simple.png" alt="Simple overview of what DVMRP is">

Table of Contents
-----------------

* [Introduction](#introduction)
* [Running](#running)
* [Configuration](#configuration)
* [Build & Install](#build--install)
* [Building from GIT](#building-from-git)
* [Contributing](#contributing)
* [Origin & References](#origin--references)


Introduction
------------

mrouted is the original implementation of the DVMRP multicast routing
protocol, [RFC 1075][].  It only works with IPv4 networks.  For more
advanced setups, the [pimd project](https://github.com/troglobit/pimd)
or [pimd-dense project](https://github.com/troglobit/pimd-dense), for
IPv6 the [pim6sd project](https://github.com/troglobit/pim6sd) may be of
interest.

mrouted is *simple* to use.  DVMRP is derived from RIP, which means it
works stand-alone without any extra network setup required.  You can get
up and running in a matter of minutes.  Use the built-in [IP-in-IP][]
tunneling support, or GRE, to traverse Internet or intranets.

mrouted is developed on Linux and works as-is out of the box.  Other
UNIX variants should also work, but are not as thoroughly tested.

Manual pages available online:

   * [mrouted(8)][]
   * [mroutectl(8)][]
   * [mrouted.conf(5)][]


Running
-------

mrouted does not require a `.conf` file.  When it starts up it probes
all available interfaces and, after an initial 10s delay, starts peering
with any DVMRP capable neighbors.

Use [mgen(1)][], [mcjoin(1)][], or [iperf](https://iperf.fr/) to send
IGMP join packets and multicast data on the LAN to test your multicast
routing setup.  Use the `mroutectl` tool to query a running `mrouted`
for status.

For the native mrouted tunnel to work in Linux based systems, you need
to have the "ipip" kernel module loaded or as built-in:

    modprobe ipip

Alternatively, you may of course also set up GRE tunnels between your
multicast capable routers.

If you have *many* interfaces on your system you may want to look into
the `no phyint` setting in [mroute.conf(5)][].  Linux users may also
need to adjust `/proc/sys/net/ipv4/igmp_max_memberships` to a value
larger than the default 20.  mrouted needs 3x the number of interfaces
(vifs) for the relevant control protocol groups.  The kernel (Linux &
BSD) *maximum* number of interfaces to use for multicast routing is 32.

**Note:** mrouted must run with suffient capabilities, or as root.


Configuration
-------------

mrouted reads its configuration file from `/etc/mrouted.conf`, if it
exists.  You can override the default by specifying an alternate file
when invoking mrouted:

    mrouted -f /path/file.conf

mrouted can be reconfigured at runtime like any regular UNIX daemon with
`SIGHUP`, or `mroutectl restart`, to activate changes made to its
configuration file.  The PID is saved in the file `/run/mrouted.pid` for
your scripting needs.

By default, mrouted configures itself to act as a multicast router on
all multicast capable interfaces.  Hence, you do not need to explicitly
configure it, unless you need to setup tunnel links, change the default
operating parameters, disable multicast routing over a specific physical
interfaces, or have dynamic interfaces.

**Note:** you need to have IP Multicast Routing enabled in the kernel
  as well.  How this is achieved is outside the scope of this README.

For more help, see the [mrouted(8)][] and [mrouted.conf(5)][] man pages.


Build & Install
---------------

### Debian/Ubuntu

    curl -sS https://deb.troglobit.com/pubkey.gpg | sudo apt-key add -
    echo "deb [arch=amd64] https://deb.troglobit.com/debian stable main" | sudo tee /etc/apt/sources.list.d/troglobit.list
    sudo apt-get update && sudo apt-get install mrouted

### Building from Source

Download the latest official *versioned* mrouted release.  Official
releases contain all the necessary files, unlike building from GIT.
mrouted has no external dependencies except for a standard C library.

* https://github.com/troglobit/mrouted/releases

The configure script and Makefile supports de facto standard settings
and environment variables such as `--prefix=PATH` and `DESTDIR=` for the
install process.  For example, to install mrouted to `/usr`, instead of
the default `/usr/local`, and redirect install to a package directory in
`/tmp`:

    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var
    make
    make DESTDIR=/tmp/mrouted-4.0-1 install-strip

This version of mrouted has [RSRR][] support for running RSVP, disabled
by default.  Enable with `configure --enable-rsrr`.

**Note:** On some systems `--runstatedir` may not be available in the
  configure script, try `--localstatedir=/var` instead.


Building from GIT
-----------------

If you want to contribute, or simply just try out the latest but
unreleased features, then you need to know a few things about the
[GNU build system][buildsystem]:

- `configure.ac` and a per-directory `Makefile.am` are key files
- `configure` and `Makefile.in` are generated from `autogen.sh`
- `Makefile` is generated by `configure` script

To build from GIT you first need to clone the repository and run the
`autogen.sh` script.  This requires `automake` and `autoconf` to be
installed on your system.

    git clone https://github.com/troglobit/mrouted.git
    cd mrouted/
    ./autogen.sh
    ./configure && make

GIT sources are a moving target and are not recommended for production
systems, unless you know what you are doing!


Contributing
------------

The basic functionality has been tested thoroughly over the years, but
that does not mean mrouted is bug free.  Please report bugs, feature
requests, patches and pull requests at [GitHub][].


Origin & References
-------------------

The mrouted routing daemon was developed by David Waitzman, Craig
Partridge, Steve Deering, Ajit Thyagarajan, Bill Fenner, David Thaler
and Daniel Zappala.  With contributions by many others.

The last release by Mr. Fenner was 3.9-beta3 on April 26 1999 and
mrouted has been in "beta" status since then.  Several prominent UNIX
operating systems, such as AIX, Solaris, HP-UX, BSD/OS, NetBSD, FreeBSD,
OpenBSD as well as most GNU/Linux based distributions have used that
beta as a de facto stable release, with (mostly) minor patches for
system adaptations.  Over time however many dropped support, but Debian
and OpenBSD kept it under their wings.

In March 2003 [OpenBSD](http://www.openbsd.org/), led by the fearless
Theo de Raadt, managed to convince Stanford to release mrouted under a
[fully free license][License], the [3-clause BSD license][BSD License].
Unfortunately, and despite the license issue being corrected by OpenBSD,
in February 2005 [Debian dropped mrouted][1] as an "obsolete protocol".

For a long time the OpenBSD team remained the sole guardian of this
project.  In 2010 [Joachim Wiberg](https://troglobit.com) revived
mrouted on [GitHub][] based on the last release by Bill Fenner, the
`mrouted-3.9beta3+IOS12.tar.gz` tarball.  This project has integrated
all (?) known patches and continuously track the OpenBSD project, which
is based on the 3.8 release, for any relevant fixes.

[1]:               http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=288112
[License]:         http://www.openbsd.org/cgi-bin/cvsweb/src/usr.sbin/mrouted/LICENSE
[License Badge]:   https://img.shields.io/badge/License-BSD%203--Clause-blue.svg
[BSD License]:     http://en.wikipedia.org/wiki/BSD_licenses
[RFC 1075]:        http://tools.ietf.org/html/rfc1075
[IP-in-IP]:        https://en.wikipedia.org/wiki/IP_in_IP
[RSRR]:            doc/RSRR.md
[buildsystem]:     https://airs.com/ian/configure/
[mgen(1)]:         https://www.nrl.navy.mil/itd/ncs/products/mgen
[mcjoin(1)]:       https://github.com/troglobit/mcjoin/
[mrouted(8)]:      https://man.troglobit.com/man8/mrouted.8.html
[mroutectl(8)]:    https://man.troglobit.com/man8/mroutectl.8.html
[mrouted.conf(5)]: https://man.troglobit.com/man5/mrouted.conf.5.html
[GitHub]:          https://github.com/troglobit/mrouted/
[Travis]:          https://travis-ci.org/troglobit/mrouted
[Travis Status]:   https://travis-ci.org/troglobit/mrouted.png?branch=master
[Coverity Scan]:   https://scan.coverity.com/projects/3320
[Coverity Status]: https://scan.coverity.com/projects/3320/badge.svg
