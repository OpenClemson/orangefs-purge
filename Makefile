# (C) 2016 Clemson University
#
# See LICENSE in top-level directory.
#
# File: Makefile
# Author: Jeff Denton

# Point this to your OrangeFS installation prior to running 'make'.
ORANGEFS_PREFIX?=/opt/orangefs

# Customize these to your liking
ORANGEFS_PURGE_LOG_DIR?=/var/log/orangefs-purge
ORANGEFS_PURGE_INSTALL_DIR?=/usr/local/sbin

USING_PINT_MALLOC?=0
# Default value for USING_PINT_MALLOC is now 0 since OFS developers seem to have
# corrected an issue present in earlier versions. OrangeFS 2.9.6 works as
# expected now. May be overridden on the command line via:
# USING_PINT_MALLOC=1 make

DEBUG_ON?=0
USE_DEFAULT_CREDENTIAL_TIMEOUT?=0

all: orangefs-purge

# Default value for USING_PINT_MALLOC is now 0 since OFS developers seem to have
# corrected an issue present in earlier versions. OrangeFS 2.9.6 works as
# expected now.
orangefs-purge: purge/src/orangefs-purge.c
	mkdir -p bin
	gcc -g -Wall -O2 \
	    -D DEBUG_ON=${DEBUG_ON} \
	    -D USE_DEFAULT_CREDENTIAL_TIMEOUT=${USE_DEFAULT_CREDENTIAL_TIMEOUT} \
	    -D USING_PINT_MALLOC=${USING_PINT_MALLOC} \
	    -o bin/orangefs-purge \
	    -I${ORANGEFS_PREFIX}/include \
	    purge/src/orangefs-purge.c \
	    -L${ORANGEFS_PREFIX}/lib \
	    -lorangefsposix

install: orangefs-purge
	install --mode=700 --directory ${ORANGEFS_PURGE_LOG_DIR}
	install --mode=700 bin/orangefs-purge ${ORANGEFS_PURGE_INSTALL_DIR}
	install --mode=700 purge/scripts/orangefs-purge-user-dirs.sh ${ORANGEFS_PURGE_INSTALL_DIR}
	install --mode=700 analytics/scripts/orangefs-purge-logs2df.py \
	    ${ORANGEFS_PURGE_INSTALL_DIR}

clean:
	rm -f \
	    bin/orangefs-purge

