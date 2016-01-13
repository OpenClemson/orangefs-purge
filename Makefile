# (C) 2015 Clemson University
#
# See LICENSE in top-level directory.
#
# File: Makefile
# Author: Jeff Denton

# Point this to your OrangeFS installation prior to running 'make'.
ORANGEFS_PREFIX=/opt/orangefs

# Customize these to your liking
ORANGEFS_PURGE_LOG_DIR=/var/log/orangefs-purge
ORANGEFS_PURGE_INSTALL_DIR=/usr/local/sbin

all: orangefs-purge

orangefs-purge: orangefs-purge.c
	gcc -g -Wall -O2 \
	    -D FILES_REMOVED_LOGGER_ENABLED=0 \
	    -D FILES_KEPT_LOGGER_ENABLED=0 \
	    -D DEBUG_ON=0 \
	    -D USE_DEFAULT_CREDENTIAL_TIMEOUT=0 \
	    -D USING_PINT_MALLOC=1 \
	    -o orangefs-purge \
	    -I${ORANGEFS_PREFIX}/include \
	    orangefs-purge.c \
	    -L${ORANGEFS_PREFIX}/lib \
	    -lorangefsposix

install: orangefs-purge
	install --mode=700 --directory ${ORANGEFS_PURGE_LOG_DIR}
	install --mode=700 orangefs-purge ${ORANGEFS_PURGE_INSTALL_DIR}
	install --mode=700 scripts/orangefs-purge-user-dirs.sh ${ORANGEFS_PURGE_INSTALL_DIR}

clean:
	rm -f \
	    orangefs-purge
