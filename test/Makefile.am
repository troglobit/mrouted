EXTRA_DIST         = lib.sh mping.c shared.sh single.sh three.sh
CLEANFILES         = *~ *.trs *.log

# Step VERSION when updating from https://github.com/troglobit/mping/
noinst_PROGRAMS    = mping
mping_CFLAGS       = -DVERSION='"1.5"'
mping_SOURCES      = mping.c

TEST_EXTENSIONS    = .sh
TESTS_ENVIRONMENT  = unshare -mrun

TESTS              = shared.sh
TESTS             += single.sh
TESTS             += three.sh
