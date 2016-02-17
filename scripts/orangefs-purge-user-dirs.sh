#!/usr/bin/env bash

# File: scripts/orangefs-purge-user-dirs.sh
# Author: Jeff Denton

# ==================================================================================================
# Usage:
# --------------------------------------------------------------------------------------------------
usage()
{
    echo "Usage: ${0} [-e <exclusions_file>] [-l <log_dir>] [-t <purge_time_threshold] users_dir" 1>&2;
    exit $1;
}
#
# NOTE Where users_dir is a required argument that is the parent directory where all of your "user
#      directories" reside.
# NOTE Where exclusions_file is a file that contains the absolute path of all the user directories
#      that you would like excluded from being purged.
# NOTE The defaults for the options can be found below under 'Configurables'.
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
#  # orangefs-purge-user-dirs.sh /mnt/orangefs
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
if [[ ${EUID} -ne 0 ]]; then
   echo "This script must be run as root!" 1>&2
   exit 1
fi

# Configurables:
# ==================================================================================================
    # The absolute path of the directory that contains all of  your "user directories".
USERS_DIR=
    # File containing a list of absolute paths of user directories that you don't want to scan with
    # orangefs-purge. This will only work for directories one level deeper than the USERS_DIR 
    # defined above.
EXCLUSIONS_LIST_FILE="/usr/local/etc/orangefs-purge-exclude"
LOG_DIR="/var/log/orangefs-purge"
declare -i PURGE_TIME_THRESHOLD=$[60 * 60 * 24 * 31] # 31 days
# ==================================================================================================

while getopts ":he:l:t:" o; do
    case "${o}" in
        e)
            EXCLUSIONS_LIST_FILE=${OPTARG}
            if ! [[ -f ${EXCLUSIONS_LIST_FILE} && -r ${EXCLUSIONS_LIST_FILE} ]]; then
                ERROR_MESSAGE="EXCLUSIONS_LIST_FILE does not exist or has incorrect permissions!"
                ERROR_MESSAGE="${ERROR_MESSAGE} EXCLUSIONS_LIST_FILE=${EXCLUSIONS_LIST_FILE}"
                echo ${ERROR_MESSAGE} 1>&2
                usage 1
            fi
            ;;
        l)
            LOG_DIR=${OPTARG}
            ;;
        t)
            PURGE_TIME_THRESHOLD=${OPTARG}
            ;;
        h)
            usage 0
            ;;
        *)
            usage 1
            ;;
    esac
done
shift $((OPTIND - 1))

# The last argument should be the "users" directory.
USERS_DIR=${1}
if ! [[ -d ${USERS_DIR} && -r ${USERS_DIR} ]]; then
    echo "USERS_DIR does not exist or has incorrect permissions! USERS_DIR=${USERS_DIR}" 1>&2
    usage 1
fi

# Make sure LOG_DIR exists and has all of the necessary permissions
if ! [[ -d ${LOG_DIR} && -r ${LOG_DIR} && -w ${LOG_DIR} && -x ${LOG_DIR} ]]; then
    echo "LOG_DIR does not exist or has incorrect permissions! LOG_DIR=${LOG_DIR}" 1>&2
    usage 1
fi

echo -e "USERS_DIR\t${USERS_DIR}"
echo -e "EXCLUSIONS_LIST_FILE\t${EXCLUSIONS_LIST_FILE}"
echo -e "LOG_DIR\t${LOG_DIR}"
echo -e "PURGE_TIME_THRESHOLD\t${PURGE_TIME_THRESHOLD}"
#exit 0

# To compute the REMOVAL_BASIS_TIME, substract the PURGE_TIME_THRESHOLD from the current time.
# Any files with both atime and mtime less than the REMOVAL_BASIS_TIME will be removed!
readonly START_TIME=$(echo $(date +%s))
readonly REMOVAL_BASIS_TIME=$(echo $[${START_TIME} - ${PURGE_TIME_THRESHOLD}])

echo -e "START_TIME\t${START_TIME}"
echo -e "REMOVAL_BASIS_TIME\t${REMOVAL_BASIS_TIME}"

# Create subdirectory pertaining to this run so the many generated log files can be easily grouped
LOG_DIR="${LOG_DIR}/${START_TIME}"
mkdir "${LOG_DIR}" && chmod u+rwx "${LOG_DIR}"

if [ -r "${EXCLUSIONS_LIST_FILE}" ]; then 
    # Build up a string of exclusions to pass to find
    cat "${EXCLUSIONS_LIST_FILE}" |
    {
        EXCLUSIONS=""
        while read dir; do
            if [ -d "${dir}" ]; then
                # WARNING Exlusion of directory paths containing whitespace will not be handled
                # correctly! As is, if your exclusions list file contains paths with whitespace,
                # the find command will probably fail and therefor no "users log" will be generated
                # which means the later cat command on it will fail and NO purging will take place.
                echo -e "excluding\t${dir}"
                EXCLUSIONS="${EXCLUSIONS} -not -path ${dir}" 
            fi
        done

        # Use find to determine all of the user directories and execute the orangefs-purge program
        # on each one using the predetermined removal-basis-time above.
        find ${USERS_DIR} -mindepth 1 -maxdepth 1 -type d ${EXCLUSIONS} -exec bash -c \
            "echo '{}' >> \"${LOG_DIR}/users-dirs\"" \;
    }
else
    # No exclusions file found.
    find ${USERS_DIR} -mindepth 1 -maxdepth 1 -type d -exec bash -c \
        "echo '{}' >> \"${LOG_DIR}/users-dirs\"" \;
fi

# Loop over the generated file and execute orangefs-purge once per line/directory.
# I'm not too concerned with wacky file names such as those with whitespace since user level
# directories are associated with a user's username which should not contain those characters.
cat "${LOG_DIR}/users-dirs" | sort |
{
    while read dir; do
        echo -e "purging\t${dir}"
        /usr/local/sbin/orangefs-purge \
            --log-dir "${LOG_DIR}" \
            --removal-basis-time=${REMOVAL_BASIS_TIME} \
            ${ORANGEFS_PURGE_EXTRA_OPTS} -- \
            "${dir}" \
            2>>"${LOG_DIR}/orangefs-purge.stderr" || >&2 printf "FAILED\t${dir}\n"
    done
}

readonly FINISH_TIME=$(echo $(date +%s))
readonly DURATION_SECONDS=$[${FINISH_TIME} - ${START_TIME}]
echo -e "FINISH_TIME\t${FINISH_TIME}"
echo -e "DURATION_SECONDS\t${DURATION_SECONDS}"
