README
======
[![Build Status](https://travis-ci.org/troglobit/mrouted.png?branch=master)](https://travis-ci.org/troglobit/mrouted)[![Coverity Scan Status](https://scan.coverity.com/projects/3320/badge.svg)](https://scan.coverity.com/projects/3320)

mrouted is a [3-clause BSD](http://en.wikipedia.org/wiki/BSD_licenses)
licensed implementation of the DVMRP multicast routing protocol.  It can
run on any UNIX based system, from embedded Linux systems to
workstations, turning them into multicast routers with tunnel support,
which can be used to cross non-multicast-aware routers.

DVMRP is a distance vector based protocol, derived from RIP, suitable
for closely located multicast users in smaller networks.  It simply
floods all multicast streams to all routers, i.e. implicit join.  This
is also known as "flood and prune" since you can opt out from groups you
do not want. For a detailed explanation of the protocol, consult
[RFC 1075](http://tools.ietf.org/html/rfc1075).


History
-------

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
[fully free license](http://www.openbsd.org/cgi-bin/cvsweb/src/usr.sbin/mrouted/LICENSE),
the [3-clause BSD license](http://en.wikipedia.org/wiki/BSD_licenses).
Unfortunately, and despite the license issue being corrected by OpenBSD,
in February 2005
[Debian dropped mrouted](http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=288112)
as an "obsolete protocol".

For a long time the OpenBSD team remained the sole guardian of this
project.  In 2010 [Joachim Nilsson](http://troglobit.com) revived
mrouted on [GitHub](https://github.com/troglobit/mrouted).  The 3.9.x
stable series represent the first releases in over a decade.  Patches
from all over the Internet, including OpenBSD, have been merged.

mrouted is primarily developed on Linux and should work as-is out of the
box on all major distributions.  Other UNIX variants should also work,
but are not as thoroughly tested.  For some tips and details, see the
`configure` script.


Building
--------

When building mrouted from source you first need to run the `configure`
script to generate the file `config.mk`.  The script relies on Bourne
shell standard features as well as expr and uname.  Any optional mrouted
features, such as `--enable-rsrr` are activated here as well.

**Example:**

    ./configure --enable-rsrr
    make

    sudo make install

The Makefile supports de facto standard environment variables such as
`prefix` and `DESTDIR` for the install process.  E.g., to install mrouted
to `/usr` instead of the default `/usr/local`, but redirect to a binary
package directory in `/tmp`:

    VERSION=3.9.7-1 prefix=/usr DESTDIR=/tmp/mrouted-3.9.7-1 make clean install


Running
-------

mrouted must run as root.

For the native mrouted tunnel to work in Linux based systems, you need
to have the "ipip" kernel module loaded or as built-in:

    modprobe ipip


Configuration
-------------

mrouted reads its configuration file from `/etc/mrouted.conf`.  You can
override the default by specifying an alternate file when invoking
mrouted:

    mrouted -f /path/file.conf

mrouted can be reconfigured at runtime like any regular UNIX daemon,
simply send it a `SIGHUP` to activate changes to the configuration file.
The PID is saved automatically to the file `/var/run/mrouted.pid` for
your scripting needs.

By default, mrouted configures itself to act as a multicast router on
all multicast capable interfaces, excluding loopback.  Therefore, you do
not need to explicitly configure mrouted, unless you need to setup
tunnel links, change the default operating parameters, disable multicast
routing over a specific physical interfaces, or have dynamic interfaces.

For more help, see the man page.


Bugs
----

The basic functionality has been tested thoroughly over the years, but
that does not mean mrouted is bug free.  Please report bugs, feature
requests, patches and pull requests in the
[GitHub issue tracker](http://github.com/troglobit/mrouted/issues)


RSRR
----

Routing Support for Resource Reservations (RSRR) is required for running
RSVP and was contributed by Daniel Zappala <daniel@isi.edu>.

RSRR allows RSVP to query mrouted for its routing entry for a particular
source-group pair.  Using the routing entry and the `IP_MULTICAST_VIF`
socket call, RSVP can forward distinct control messages out each
outgoing interface.  This version of mrouted supports RSRR messages
using a Unix datagram socket.

RSRR currently includes two pairs of query-reply messages.  RSVP sends
an Initial Query when it starts.  Mrouted responds with an Initial Reply
that includes the set of vifs it is using, flagging those that are
administratively disabled.  When RSVP needs the routing entry for a
source-group pair, it sends a Route Query.  Mrouted responds with a
Route Reply that includes the incoming vif and outgoing vifs for the
source-group pair.

RSVP may request route change notification by setting the notification
bit in the Route Query.  If mrouted can provide route change
notification for the source-group pair, it sets the notification bit in
its Route Reply.  When the routing entry for the source-group pair
changes, mrouted sends an unsolicited Route Reply containing the new
routing information.
