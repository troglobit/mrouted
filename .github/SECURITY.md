Reporting a Vulnerability
=========================

Please report security issues privately, using the *Report a vulnerability*
button under [Security][advisories] on GitHub.  That opens a draft advisory
only you and the maintainer can see, and it is where a CVE is requested if one
is warranted.

If you cannot use GitHub, email the maintainer directly, see the AUTHORS
section of the [mrouted(8)][manual] manual page.  Please do not open a public
issue for something you believe is exploitable until a fix is available.

Include the version, or git commit, you tested, the configuration needed to
reach the problem, and how to trigger it.  A packet capture or a small
reproducer is very welcome.  You will be credited in the advisory and in the
[ChangeLog][], unless you would rather not be.

Supported Versions
------------------

Only the latest release is supported.  Fixes go on the master branch and into
the next release; there are no separate maintenance branches, and no backports
to older releases.  Distributors are welcome to cherry-pick.

mrouted implements DVMRP, a protocol from 1988 which has no authentication of
any kind.  Any host able to send IGMP to the router is able to speak DVMRP to
it, so packet parsing bugs are usually remotely triggerable by design.  Use
the `phyint` and `tunnel` settings to limit where mrouted listens, and filter
protocol 2 at your borders.

[ChangeLog]:  https://github.com/troglobit/mrouted/blob/master/ChangeLog.md
[advisories]: https://github.com/troglobit/mrouted/security
[manual]:     https://man.troglobit.com/man8/mrouted.8.html
