Change Log
==========

All notable changes to the project are documented in this file.

[v4.4][] - 2021-11-03
---------------------

### Changes
- Rename tunnel vifs, from base interface, to use the Linux kernel
  naming; `dvmrpN`, where `N` is the VIF number.  Other kernels may
  handle this differently, patches to support other nomenclatures are
  most welcome!
- Logging to stdout now always prefixes messages with the daemon ident
- If adding a tunnel VIF and Linux does not have `ipip.ko` loaded, 
  mrouted logs this as a warning message
- Add test for IPIP tunnels
- Add Docker container image, see https://ghcr.io/troglobit/mrouted
- Update mping testing tool to v1.6 (internal)
- Refactored linked-list handling in unicast route engine (internal)
- Drop experimental RSRR feature.  It is very likely unused these days,
  seeing as the draft memo never made it into widespread use.  It is
  also not working properly with multiple instances of mrouted

### Fixes
- Issue #52: IP-IP tunnels don't work anymore.  Somewhere in the big
  refactor for the mrouted v4.x series, the `tunnel` directive in the
  .conf parser was never adapted to the new internals
- Fix a 10 year old regression after a linked-list refactor, causing
  off-by-one (loss of one) in unicast route distribution.  Which in
  turn cause VIF tunnels to malfunction


[v4.3][] - 2021-09-19
---------------------

### Changes
- Add support for `-i,--ident=NAME` to change identity of an instance
- Add support for `-p,--pidfile=FILE` to override default PID file
- Touch PID file at SIGHUP to acknowledge done reloading .conf file
- Add support for `-t,--table-id=ID`, multicast routing tables (Linux)
- Add support for `-u,--ipc=FILE` to override `/var/run/mrouted.sock`
  file, used for communication with `mroutectl`

### Fixes
- Fix segfault when parsing `phyint` lines in .conf file interface
  cannot be found, e.g., `phyint eth1 static-group 225.1.2.5`
- Prevent cascading warnings when phyint interface names cannot be found


[v4.2][] - 2021-01-07
---------------------

Major bug fix and feature release.  Support for static routes and
improved configuration support for IGMP.

### Changes
- Support for controlling IGMP Last Member Query Count using the
  `igmp-robustness` setting in `mrouted.conf`, default 2
- Support for tuning the IGMP Last Member Query Interval using a
  new setting `igmp-query-last-member-interval <1-1024>`.  Issue #44
- Support for static multicast routing (*,G), similar to SMCRoute.
  New `phyint static-group GROUP` setting in mrouted.conf, multiple
  statements supported, but no ranges (yet).  Issue #31
- Proper tracking of lower-version host members (IGMP), when a lower
  version host is detected for a group, a timer is set according to
  RFC3376, and while in this compat mode higher-version IGMP is not
  allowed to change state.  E.g., in IGMPv1 compat, IGMPv2 LEAVE is
  ignored for the group, similar to the phyint being in `igmpv1` mode
- Allow IGMP reports from source address 0.0.0.0, required as per
  RFC3376, sec. 4.2.13, not supported until now.  This should greatly
  improve interop with IGMP snooping switches and DHCP clients that
  have not yet received a lease
- Improved support for running mroutectl under watch(1).  No more
  artifacts due to unknown ANSI escape sequences to probe width
- Delayed PID file creation until after initial startup delay, there
  is nobody home until after that delay, so no point in announcing
  availability until after that

### Fixes
- Issue #43: IGMPv3 membership reports were parsed incorrectly.  The
  problem affects users that use source specific multicast join, i.e.,
  (S,G) join/leave using IGMPv3.  Support for IGMPv3 was introduced in
  mrouted [v4.0][]
- Issue #46: Malformed group-specific IGMP query.  The IGMP header no
  longer had the group field set, despite the query being addressed to
  a specific group.  Regression introduced in [v4.0][]
- Issue #47: The optional phyint flag `igmpv3` did not work.
- Fix buffer overrun in descriptor `poll()` handling
- Fix double-close on SIGHUP, Linux systems only
- Various non-critical memory leak fixes, critical for no-MMU systems


[v4.1][] - 2020-10-02
---------------------

Minor feature and bug fix release.

### Changes
- Issue #40: Automatically detect and add `altnet` to interfaces with
  multiple addresses, possible thanks to work on #36
- Reduce number of exposed aliases to debug sub-systems in online help
  text and man page.  Only primary name, as of mrouted v3.9-beta3
- Removed noisy `timer` sub-system from `-d all`, use `-d all, timer`
- Document a lot of `mrouted.conf` options available in this version of
  mrouted since before v3.9, but not in the OpenBSD, based on v3.8:
  - `prune-lifetime`
  - `rexmit-prunes`
  - `phyint` and tunnel interface flags:
	- `advert-metric`
	- `allow-nonpruners`
	- `blaster`
	- `force-leaf`
	- `noflood`
    - `passive`
	- `prune-lifetime`
	- `rexmit-prunes`
  - The tunnel option `beside off`
  - Router filtering options with `accept`, `deny`, and `notransit`

### Fixes
- Fix update of `mrouted.genid` on SIGHUP and reboot.  mrouted replaced
  contents with the value zero (0), causing a zero genid in DVMRP as
  well, which likely caused peering issues with some implementations
- Fix build warning on Clang 3.4.1 (FreeBSD 10.3)
- Workaround for older autoconf without `--runstatedir` support
- Fix double free in `pidfile()`
- Fix #35: Cannot disable multicast routing in kernel: Permission denied
  when starting up.
- Fix #36: Refactor interface probing and bringup.  Fixes issue with the
  `no phyint` config option not working, introduced in v4.0
- Fix #37: Fix bad path for mrouted.genid, should be in `/var/lib/misc`
  on Linux and `/var/db` on *BSD
- Fix #38: Document and improve error message when running out of IGMP
  groups on Linux.  When running with many interfaces
- Fix #40: Detect and warn if multicast ingresses an unknown vif


[v4.0][] - 2020-06-09
---------------------

Major release with full IGMPv3 (ASM) support and a new `mroutectl` tool.

**Note:** command line options have been changed!

### Changes
- Support for IGMPv3, both sending queries and accepting membership
  reports, issue #16
- Support for configurable IGMP query interval, issue #26
- Support for configurable IGMP robustness variable, issue #27
- *Incompatible* command line option refactor
- New directive in `mrouted.conf`: `no phyint`, reverses the default
  behavior of `mrouted`.  Interfaces can then selectively be enabled

        no phyint
        phyint eth1 enable
        phyint eth2 enable

- Support for disabling the IP router-alert option:

        no router-alert

- Add systemd unit file
- Introduce `mroutectl`, a helpful tool to interact with `mrouted`.
  This completely replaces `mrouted.cache` and `mrouted.dump`, including
  `SIGUSR1` and `SIGUSR2` signals, which are now ignored, issue #24
- The `mrouted.pid` file, and the new `mrouted.sock` file, are now
  located in `/var/run`
- Major cleanup of logging directives read from the command line, and
  from `mroutectl`.  Use `-d ?`, and `-l ?` to list alternatives
- GNU Configure & Build system, use `./autogen.sh` only when building
  directly from GIT sources, otherwise use `./configure` from tarball

### Fixes
- Fix #20: Replace obsolete `gethostbyname()` w/ `getaddrinfo()`
- Fix #25: Save `mrouted.genid` to persistent store in `/var/lib`
  instead of `/var/run`
- Fixed libc portability issues, e.g. GNU:isms like `%m` etc.
- Import OpenBSD fix to `daemon()` equivalent, use `/dev/null` for
  stdin, stdout and stderr
- Use `clock_gettime()`, with monotonic clock, instead of the unsafe
  `gethostbyname()`, for all non-date-printing code paths.  Only for
  mrouted, other tools have not been changed
- Fix lots of invalid format specifiers, found by Coverity Scan and
  clang on FreeBSD
- Fix detection of `netinet/igmp.h` on FreeBSD
- Fix memory leaks in `mrouted` on `SIGHUP`


[v3.9.8][] - 2017-01-01
-----------------------

#### Changes
- New option `-D` or `--startup-delay` to tune the initial delay
  during which routes are exchanged, but not applied.
- The mrouted man page has been cleaned up and sections clarfied.
- Add `-D_DEFAULT_SOURCE` for building on GLIBC v2.20, and later.
- Sync with OpenBSD mrouted

### Fixes
- Matt Weber found and fixed a serious bug with DVMRP reports missing
  subnet (off by one error) which seems to have been introduced in
  v3.9.5.  Issue #14
- Fix mtrace compilation with Clang 3.5, fix courtesy of FreeBSD,
  <https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=196166>, by Dimitry
  Andric (@DimitryAndric), heads-up by Olivier Cochard-Labbé (@ocochard)
- Minor warnings from scan-build (clang) also fixed.  See the GIT log
  for more details.


[v3.9.7][] - 2014-12-28
-----------------------

### Changes
- SNMP Support removed.  It never compiled and would have likely
  needed a complete refactor to support modern AgentX.
- Replaced static `config.mk` with configure script from pimd
- RSRR Support disabled by default, use `--enable-rsrr` to configure script
- Add `'enable'` to `phyint` directive and `-M/-N` command line options,
  thanks to Joseph Gooch (@goochjj)
- Add David Waitzman and Craig Partridge to list of original authors
  after being contacted by Mr Waitzman :)
- Change to use `stdint.h` types instead of type unsafe homegrown types

### Fixes
- Fix issue with older BSD kernels, mainly for current FreeBSD 10 and
  older, that don't really give RAW sockets but byte swap `ip_len`
  field, Olivier Cochard-Labbé (@ocochard)
- Build fixes for FreeBSD, should make maintaining ports easier :)
- Change from `select()` to `poll()` due to descriptor limits, e.g.,
  on BSD.
- UNIX 2038 first audit, inspired by OpenBSD.  Cleanup type confusion
  `int/u_long` where it should be `time_t`.  Also, clarify that `genid`
  is OK since it is used and stored as an unsigned 32-bit integer.
- Lots of minor fixes detected by Coverity Scan and Clang scan-build
  https://scan.coverity.com/projects/3320


[v3.9.6][] - 2011-10-23
-----------------------

### Changes
- The Makefile now accepts `CFLAGS` from the environment instead of
  simply overriding.  The old `USERFLAGS` variable, previously intended
  for this purpose, is still supported for backwards compatibility
  reasons.

### Fixes
- Serious regression in route.c, introduced in 3.9.5, caused by the link
  list refactor. Fix by Seth Hinze <ultrix@gmail.com>
- Fix GCC 4.6 warnings for unused variables.


[v3.9.5][] - 2011-03-05
-----------------------

### Changes
- The location of dump files have been moved from `/var/tmp` to
  `/var/run/mrouted` due to the insecure nature of `/var/tmp`.  See more
  below.
- Add `-r,--show-routes` which sends `SIGUSR1` to a running daemon,
  waits for the file `/var/run/mrouted/mrouted.dump` to be updated, and
  then displays the result on stdout.

### Fixes
- The linked list implementation used in `route.c` caused several
  problems and as a result has been refactored.  This fixes several
  `SIGSEGV` crashes a couple of memory leaks as well as GitHub issue #7.
- Ported from pimd after CVE-2011-0007: Insecure file creation in `/var/tmp`:  
  *"On USR1, pimd will write to `/var/tmp/pimd.dump` a dump of the
    multicast route table. Since `/var/tmp` is writable by any user, a
    user can create a symlink to any file he wants to destroy with the
    content of the multicast routing table."*


[v3.9.4][] - 2010-11-19
-----------------------

### Fixes
- `kern.c:k_del_vif()` does not work properly in Linux.

  When some interface (known by mrouted) goes down, mrouted tries to
  remove related VIF by calling `stop_vif()`, which in turn calls
  `k_del_vif()`.  After `k_del_vif()` is called, mrouted exits with the
  following error:

  *"setsockopt `MRT_DEL_VIF` on vif 3: Invalid argument"*

  The reason for this is due to differences in the Linux and *BSD
  `MRT_DEL_VIF` API.  The Linux kernel expects to receive a `struct
  vifctl` associated with the VIF to be deleted, *BSD systems on the
  other hand expect to receive the index of that VIF.

  Fix contributed by Dan Kruchinin <mailto:dkruchinin@acm.org>


[v3.9.3][] - 2010-10-11
-----------------------

### Changes
- Update man page with `--long-options`, missing sections and improve
  debug help.
- Cleanup Makefile for use with BSD PMake as well as GNU Make.

### Fixes
- Fix `NULL` pointer dereference in conf file parser.  Problem will
  arise for all interfaces that at one point might not have an address.

  Reported by Dan Kruchinin <mailto:dkruchinin@acm.org>
- Fix problem with running the tunnel directive on OpenVPN, PPTP, L2TP
  tunnels as well as PPP links.  All of which use a 255.255.255.255
  netmask on their interfaces.

  See http://openvpn.net/archive/openvpn-users/2004-04/msg00003.html for
  original problem report.

  Fix contributed by Dan Kruchinin <mailto:dkruchinin@acm.org>
- `route.c:accept_probe()`: Fix missing check of `malloc()` return value.
- `vif.c:SetTimer()`: Dito.
- `route.c:accept_report()`: Fix potential stack overflow issue.  Also
  added checks to prevent overstepping array boundaries in local `rt[]`
  array when parsing route report messages.


[v3.9.2][] - 2010-08-16
-----------------------

### Changes
- Reduce code duplication on platforms carrying `strlcpy()` and `strtonum()`.

### Fixes
- Fix file paths for GNU/Linux installations, they too use `/var/tmp`
  rather than `/usr/tmp` today.
- Code fixes in RSRR code (disabled by default).
- Fix possible build error in strtonum.c on platforms not supporting
  `LLONG_MIN/MAX`


[v3.9.1][] - 2010-04-10
-----------------------

Biggest news in this release is that all OpenBSD patches as of this date
are merged.

### Changes
- Change license to 3-clause BSD on mrinfo, RSRR and mrouted sources,
  thanks to hard working OpenBSD team!
- Support for older yacc versions.

### Fixes
- OpenBSD, all patches from their CVS repository have been merged.
  Things like missing free for malloc, missing checks for malloc return
  value, restart syscalls after signal (`EINTR`).  As well as a heap of
  neat code cleanup and modernization.


[v3.9.0][] - 2010-01-23
-----------------------

### Changes
- Debian, build fixes for GNU/Linux.
- FreeBSD ports collection, major API cleanups.
- Buildroot, some minor cleanups of old deprecated APIs
- Philippe Troin <mailto:phil@fifi.org>, added more compiler warnings
  and fixed the problems uncovered by that.


v3.9-beta3 - 1999-04-26
-----------------------

### Changes
- A `blaster` keyword for mrouted.conf, to turn on handling of routers
  (mostly ciscos) which overwhelm the socket buffers by blasting the
  whole routing table at once.
- A `notransit` keyword; routes learned on a `notransit` vif will not be
  readvertised onto another `notransit` vif.
- The 500 kbps default rate limit on tunnels has been removed.
- An ICMP listener which logs ICMP errors which appear to be in response
  to tunnel packets that we sent.
- A tunnel traffic encapsulator, which encapsulates control traffic
  inside the tunnel instead of unicasting it `beside` the tunnel.  This
  is turned off by default; use `beside off` to turn it on.
- A `force_leaf` flag to ignore any potential neighbors on a given
  interface.

### Fixes
- There was a bug handling routing updates which caused random black
  holes.
- There was a race condition in the timer handlers causing free'd memory
  to sometimes get touched.
- `allow_nonpruners` wasn't allowed in the configuration file (and
  almost nobody noticed! - probably a good sign)
- When a prune times out and the source has been active "recently",
  mrouted now waits for further traffic instead of triggering a new
  prune.
- mrouted now ignores unreachable routes when making a routing decision
  (previously it would blackhole, now it can find a less-specific)


v3.9-beta2 - 1997-06-11
-----------------------

There is no need to upgrade to 3.9-beta2 if you are not experiencing one
of the following bugs.

### Fixes
- There was a bug in 3.9-beta1's raw socket buffer processing that
  would cause an immediate lockup on startup on some systems.
- RSRR would not clear out the group membership information if
  further notification of changes to this route entry was not possible.


v3.9-beta1 - 1997-06-06
-----------------------

### Changes
- Longer prune lifetimes (2 hours) by default.  Prune lifetimes may be
  configured per-vif, with the `prune_lifetime N` mrouted.conf
  configuration file entry (where N is in seconds).  This helps to work
  around the black holes caused on restart when you have a Cisco
  upstream which does not handle genid's; if this is your situation the
  recommended value is 300.
- mrouted's behavior of flooding new routes by default at startup in
  order to speed healing of paths during startup can be turned off
  per-vif or globally with the `noflood` configuration option.  Turning
  this option off means you are likely to experience black holes for a
  minute or two when you restart a router.  The default is to flood for
  a minute or two until mrouted is able to learn subordinate
  relationships.
- mrouted now retransmits prunes by default on point-to-point links.
  The mrouted.conf command `rexmit_prunes [on|off]` can be used to
  enable or disable this feature on a per-vif basis.  Prune
  retransmission helps on lossy links, and also helps when a router has
  forgotten about a prune (e.g. if it is out of memory and needs to shed
  state, or due to a bug).
- The new `passive` mode causes mrouted to not actively send probes
  looking for neighbors.  This allows a dialup link to become quiescent
  if there is no DVMRP neighbor on the other end.  Configuring `passive`
  on both ends of a link will cause it to never come up.
- mrouted defaults to not peering with DVMRP routers that do not prune.
  Use the `allow_nonpruners` mrouted.conf option on a vif on which you
  want to allow such peerings.
- mrouted now allows route filtering using `allow` and `deny` in
  `mrouted.conf`:
  - Only `accept` or `deny` is allowed, no combinations.
  - Add `bidir` to apply the filter to output too, otherwise it's input
    only.
  - Expected usage:
    - Providers filter routes that customers send them
    - Martian removal
    - Topology modification (e.g. don't let the existence of private
      tunnel foo out into the world).
  - Syntax:
      - accept 13/8 :: All routes matching 13/8 (e.g. 13.2.116/22)
      - accept 13/8 exact :: If you want to accept exactly 13/8
      - deny 10/8 64/2 130/8 exact 172/8 exact :: Common MBone martians
- mrouted now malloc's the buffer it uses for `SIOCGIFCONF`, to allow
  for more interfaces.  Thanks to Danny Mitzel
- mrouted now ignores multiple entries for a single interface name
  (temporary hack until mrouted understands interface aliases)
- mrouted's `-d` flag has been modified to accept the names of the
  systems which you would like to debug: packet, prunes, routes, peers,
  cache, timeout, interface, membership, traceroute, igmp
- mrouted now times neighbors out fater, and fully detects and ignores
  routes from one-way peerings.
- mrouted's route processing has been sped up, especially at startup.
- mrouted uses the biggest `SO_RCVBUF` the operating system allows (up to 256 kb)
- mrouted uses TOS `0xC0` ("Internet Control") for DVMRP messages.

### Known Issues
- The startup message doesn't print properly if you have too many
  interfaces.

### Fixes
- mrouted did not properly keep track of subordinates, and would not
  time out subordinateness.  This caused 2 major problems:
  1. pruning did not happen when there were equal-cost paths to the same
     multi-access link
  2. subordinateness which did not get cancelled by a non-poisoned route
     (e.g. in the face of route filtering) did not time out, causing
     traffic to continue to flow.
- mrouted's IGMPv2 processing when it is not the querier now conforms to
  draft-ietf-idmr-igmp-v2-06.txt Thanks to Lorenzo VICISANO
  <mailto:L.Vicisano@cs.ucl.ac.uk> for finding a problem.
- mrouted is much more careful about forgetting prunes; 3.8 would forget
  prunes whenever any route change ocurred.


v3.8 - 1995-11-29
-----------------

### Fixes
- mrouted would fail to forget prunes when a neighbor went away, thus
  potentially sending traffic down a tunnel after the tunnel endpoint
  has gone down.  This was due to some research code making it into the
  "emergency" 3.7 release, sigh.
- mrouted could send prunes with negative lifetimes.  This causes
  slightly higher prune traffic but shouldn't be any major problem.

### Manifest

| README-3.8.mrouted | this file                                             |
|--------------------|-------------------------------------------------------|
| mrouted/*          | version 3.8 of mrouted, mrinfo, map-mbone and mtrace. |
| ifconfig/*         | Changes to ifconfig to show multicast interfaces      |
| netstat/*          | Diffs to netstat                                      |
| ping/*             | sources for ping which support multicasting           |
| mtest/*            | utility for testing multicast group membership        |


v3.7 - 1995-11-28
-----------------

### Changes
- The configuration file can accept a hostname as the other end of a
  tunnel.  There must be a single name-to-ip mapping for the given name,
  however, or mrouted will fail to start up.
- mrinfo now sends requests to all interfaces of a multihomed host.
- mtrace's passive mode has been implemented.
- The first screen of mtrace statistics is shorter and more likely
  to fit on one screen.

### Fixes
- mrouted now ignores route reports that include bogus netmasks.
  There was a bug in 3.5 that would mangle default routes into
  tens of bogus routes; this should prevent that bug from killing
  the MBONE.

  This solution can cause route flaps and black holes until the
  3.5's are gone or all of the 3.5's neighbors are 3.7 .
- mrouted now ignores duplicate routes.  Ciscos and the above 3.5
  bug could cause two copies of the same route to appear in a single
  routing update; mrouted would insert two copies of the same route
  into its routing table and wreak all sorts of havoc.
- mrouted now sends a group-specific query for both retransmissions
  of a g-s query; previous versions sent a general query the second
  time.
- mrouted now loops back multicasted mtrace responses and
  group-specific membership queries
- mrouted now performs deterministic tiebreaking between two
  neighbors on the same vif.
- mrouted now only does duplicate suppression on traceroute requests,
  not all traceroute packets, so that a loop can be nicely detected
  via a duplicate router instead of just a timeout.
- the buffer size that mrouted uses has been increased to allow
  more than 16 hops in mtrace messages.
- mtrace's hop-by-hop termination is now more likely to be correct.
- mrinfo now waits for the responses to its retransmitted queries.


v3.6 - 1995-06-26
-----------------

### Fixes
- mrouted would dump core when attempting to report no routes (i.e. upon
  startup, if you have no enabled phyint's)
- mrouted would dump core if requested to traceroute a source for which it
  had no route
- neighbor flags were not always properly updated on probe or report
- mrouted would sometimes reply to a multicast traceroute on a disabled
  phyint; now it uses the first configured phyint to reply to traceroutes.
- host routes (i.e. netmask `0xffffffff`) works now; it was discarding
  IGMP from the host because it was coming from the "broadcast address"
  of the subnet.
- `send_igmp()` now treats the failure to send an mtrace or a neighbor
  reply as informational, as opposed to warning.
- mrouted would go into an infinite loop trying to respond to a traceroute
  for a source with a netmask of `0xffffffff`.
- `vifs_with_neighbors` was not being reset if the mrouted was restarted
  with `SIGHUP`.
- the default route was not being properly advertised to neighbors (although
  it was accepted if it was advertised to it)
- ANSI-fication for those who it helps, still-K&R-ish for those it doesn't.
- mtrace now attempts to trace three hops past a non-responding router,
  in the hopes that it does support traceroute but just couldn't respond
  (i.e. unicast didn't work and it can't source multicast because all its
  phyints are disabled).
- mrinfo now times out even on a multicast router.


v3.5 - 1995-05-08
-----------------

### Changes
- The kernel and mrouted make sure that each is the correct version, to
  prevent problems with mismatched kernel/mrouted versions.  A too-old
  mrouted will die with the error:  
  "can't enable DVMRP routing in kernel: Option not supported by protocol"
- mrouted can accept and propogate a default route (essential for
  heirarchical multicast routing)
- Kernel route cache keeps source-specific routes instead of subnet routes,
  eliminating hashing and longest-match problems.
  (allows classless routing, longest-match and default routing)
- Cached kernel routes only get deleted if no traffic is flowing, to
  facilitate multicast traceroute
- mrouted has a new configuration file parser, which provides better error
  messages than before, and allows named boundaries (see man page)
- added `netmask` to phyint configuration, at the suggestion of
  Anders Klemets
- System V and FreeBSD compatibility from John Brezak <mailto:brezak@ch.hp.com>
- phyint's can have additional subnets configured, for people with multiple
  subnets on one physical network.  mrouted.conf syntax is altnet 1.2.3.0,
  or altnet 1.2.3.0/24 if you need to specify a different netmask.  There
  can be as many altnet statements as you need.
- both mrouted and the kernel now support classless addresses.
- the kernel supports PIM assert processing by notifying the router
  when a packet arrives on the wrong interface
- the kernel keeps additional counters, and mrouted can be compiled to
  support SNMP and the Multicast MIB
- the packet classifier in the kernel now uses the following udp port
  ranges.  A future release of a session directory will allocate ports in
  these ranges:
  - `[0, 16384)`: lowest priority, unclassified
  - `[16384, 32768)`: highest priority, i.e. audio
  - `[32768, 49152)`: medium priority, i.e. whiteboard
  - `[49152, 65536)`: low priority, i.e. video
- the configuration code has been modified to default tunnels' `rate_limit`
  parameters to 500kbps.  This is easily modified with a `rate_limit` keyword
  in mrouted.conf, but should be a good default for the MBONE in general.
- The tunnel sending code now caches a route for `ip_output()`, this should
  help performance on machines with lots of tunnels.
- Dispatching for de-capsulating packets is now via protosw[], making
  reception of other raw protocols more efficient
- Neighbor capabilities are discovered via a bitmask as opposed to
  version number.
- Multicast traceroute code improved
- mrouted can be compiled with Routing Support for Resource Reservation
  (RSRR), required for RSVP.

### Fixes
- The IGMPv2 query timeout field was interpreted as being in units of 200ms
  as opposed to 100ms, thus the maximum timeout was set to twice the
  expected value.  This is not fatal, as mrouted always queries twice in the
  expectation that a packet could get loss, but it does make it less robust
  in the face of packet loss.
- IGMP could report membership in local-only groups (i.e. 224.0.0.X)
- IGMP could get confused by hearing its own new membership reports, thus
  a router would never perform fast leave.
- IGMP could reset timers for the wrong interface.
- mrouted put a bogus value in the maximum timeout field of IGMPv2 query
  packets.
- Non-querier mrouters would respond to IGMP leave messages
- mrouted was not performing fast leave properly
- If the last member goes away on a transit network, the upstream router
  would stop forwarding even if there are downstream members.
- Kernel hash function improved
- Eliminated possibility of `panic()`: timeout in cache maintenance
- Reordered resource allocation when sending upcall to handle failure properly
- some endian-ness bugs squashed in mrouted, probably more to go.
- Multicast traceroute could send a reply on a disabled interface.


[UNRELEASED]: https://github.com/troglobit/mrouted/compare/4.4...HEAD
[v4.4]:       https://github.com/troglobit/mrouted/compare/4.3...4.4
[v4.3]:       https://github.com/troglobit/mrouted/compare/4.2...4.3
[v4.2]:       https://github.com/troglobit/mrouted/compare/4.1...4.2
[v4.1]:       https://github.com/troglobit/mrouted/compare/4.0...4.1
[v4.0]:       https://github.com/troglobit/mrouted/compare/3.9.8...4.0
[v3.9.8]:     https://github.com/troglobit/mrouted/compare/3.9.7...3.9.8
[v3.9.7]:     https://github.com/troglobit/mrouted/compare/3.9.6...3.9.7
[v3.9.6]:     https://github.com/troglobit/mrouted/compare/3.9.5...3.9.6
[v3.9.5]:     https://github.com/troglobit/mrouted/compare/3.9.4...3.9.5
[v3.9.4]:     https://github.com/troglobit/mrouted/compare/3.9.3...3.9.4
[v3.9.3]:     https://github.com/troglobit/mrouted/compare/3.9.2...3.9.3
[v3.9.2]:     https://github.com/troglobit/mrouted/compare/3.9.1...3.9.2
[v3.9.1]:     https://github.com/troglobit/mrouted/compare/3.9.0...3.9.1
[v3.9.0]:     https://github.com/troglobit/mrouted/compare/mrouted-3.9beta3+IOS12...3.9.0
