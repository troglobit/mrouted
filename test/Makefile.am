EXTRA_DIST         = lib.sh mping.c pod.sh shared.sh single.sh three.sh
CLEANFILES         = *~ *.trs *.log

noinst_PROGRAMS    = mping
mping_SOURCES      = mping.c

TEST_EXTENSIONS    = .sh
TESTS_ENVIRONMENT  = unshare -mrun --map-auto

TESTS              = pod.sh
TESTS             += shared.sh
TESTS             += single.sh
TESTS             += three.sh
TESTS             += tunnel.sh
