#!/usr/bin/env bash

# File: scripts/orangefs-purge-user-dirs.sh
# Author: Jeff Denton
# Last Updated: 12/31/2015

# ==================================================================================================
# Usage:
# --------------------------------------------------------------------------------------------------
#   # orangefs-purge-user-dirs.sh [users_dir] [exclusions_file]
#
# NOTE Where users_dir is the parent directory where all of your "user directories" reside.
# NOTE Where exclusions_file is a file that contains the absolute path of all the user directories
#      that you would like excluded from being purged.
# NOTE The defaults for those options can be found below under 'Configurables'.
#
# Example:
#
# If you want to purge all of the directories under /mnt/orangefs, but exclude /mnt/orangefs/ike and
# /mnt/orangefs/david, then your exlusions file would contain:
# --------------------------------------------------------------------------------------------------
# /mnt/orangefs/ike
# /mnt/orangefs/david
#
# Sample execution and output (output will be tab delimited key value pairs):
# --------------------------------------------------------------------------------------------------
#  # orangefs-purge-user-dirs.sh
#  START_TIME      1451576306
#  REMOVAL_BASIS_TIME      1448897906
#  excluding       /mnt/orangefs/david
#  excluding       /mnt/orangefs/ike
#  purging /mnt/orangefs/ava
#  purging /mnt/orangefs/ben
#  FAILED  /mnt/orangefs/ben
#  purging /mnt/orangefs/corey
#  purging /mnt/orangefs/erica
#  FAILED /mnt/orangefs/erica
#  purging /mnt/orangefs/fred
#  purging /mnt/orangefs/harry
#  purging /mnt/orangefs/jeff
#  FINISH_TIME     1451576322
#  DURATION_SECONDS        16
#
# NOTE If the orangefs-purge program returns an error code you will see the FAILED message and the
# directory that failed to be purged."
# ==================================================================================================

# Must be run as root!
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root!" 1>&2
   exit 1
fi

# Configurables:
# ==================================================================================================
    # The absolute path of the directory that contains all of  your "user directories".
readonly USERS_DIR=${1:-"/mnt/orangefs"}
    # File containing a list of absolute paths of user directories that you don't want to scan with
    # orangefs-purge. This will only work for directories one level deeper than the USERS_DIR 
    # defined above.
readonly EXCLUSIONS_LIST_FILE=${2:-"/usr/local/etc/orangefs-purge-exclude"}
readonly PURGE_WINDOW=$(echo $[60 * 60 * 24 * 31]) # 31 days
readonly LOG_DIR="/var/log/orangefs-purge"
# ==================================================================================================


# Using the current time when this script is executed compute the REMOVAL_BASIS_TIME by subtracting
# a length of time (in seconds). Any files with both atime and mtime less than the
# REMOVAL_BASIS_TIME will be removed!
readonly START_TIME=$(echo $(date +%s))
readonly REMOVAL_BASIS_TIME=$(echo $[$START_TIME - $PURGE_WINDOW])

echo -e "START_TIME\t$START_TIME"
echo -e "REMOVAL_BASIS_TIME\t$REMOVAL_BASIS_TIME"

if [ -r "$EXCLUSIONS_LIST_FILE" ]; then 
    # Build up a string of exclusions to pass to find
    cat "$EXCLUSIONS_LIST_FILE" |
    {
        EXCLUSIONS=""
        while read dir; do
            if [ -d "$dir" ]; then
                # WARNING Exlusion of directory paths containing whitespace will not be handled
                # correctly! As is, if your exclusions list file contains paths with whitespace,
                # the find command will probably fail and therefor no "users log" will be generated
                # which means the later cat command on it will fail and NO purging will take place.
                echo -e "excluding\t$dir"
                EXCLUSIONS="$EXCLUSIONS -not -path $dir" 
            fi
        done

        # Use find to determine all of the user directories and execute the orangefs-purge program
        # on each one using the predetermined removal-basis-time above.
        find $USERS_DIR -mindepth 1 -maxdepth 1 -type d $EXCLUSIONS -exec bash -c \
            "echo '{}' >> \"$LOG_DIR/users-dirs-$START_TIME.log\"" \;
    }
else
    # No exclusions file found.
    find $USERS_DIR -mindepth 1 -maxdepth 1 -type d -exec bash -c \
        "echo '{}' >> \"$LOG_DIR/users-dirs-$START_TIME.log\"" \;
fi

# Loop over the generated file and execute orangefs-purge once per line/directory.
# I'm not too concerned with wacky file names such as those with spaces since user level
# directories are associated with a user's username.
cat "$LOG_DIR/users-dirs-$START_TIME.log" | sort |
{
    while read dir; do
        # TODO enable passing of LOG_DIR to orangefs-purge
        echo -e "purging\t$dir"
        DRY_RUN=1 /usr/local/sbin/orangefs-purge \
            --removal-basis-time=$REMOVAL_BASIS_TIME \
            "$dir" \
            2>>"${LOG_DIR}/orangefs-purge.stderr" || >&2 printf "FAILED\t$dir\n"
    done
}

readonly FINISH_TIME=$(echo $(date +%s))
readonly DURATION_SECONDS=$[$FINISH_TIME - $START_TIME]
echo -e "FINISH_TIME\t$FINISH_TIME"
echo -e "DURATION_SECONDS\t$DURATION_SECONDS"
