/*
 * (C) 2016 Clemson University
 *
 * See LICENSE in top-level directory.
 *
 * File: purge/src/orangefs-purge.c
 * Author: Jeff Denton
 *
 * Usage:
 * -------------------------------------------------------------------------------------------------
 * This program will remove files not read nor written in the past 31 days from the time this
 * program is executed. It must be run as root, so proceed with caution. Do not type the # character
 * shown in the examples to follow. Also be sure that you DO NOT include a trailing '/' in the path
 * argument.
 *
 *     # orangefs-purge [OPTIONS]... /path/of/orangefs/directory/to/be/purged
 *
 * To understand the effects of running this program without deleting any files, pass the
 * -d or --dry-run flag or set the DRY_RUN environment variable to 1. This will behave exactly as
 * above except the files will not be removed. This will help you understand what might be removed
 * as a result of a later true purge.
 *
 *     # orangefs-purge --dry-run /path/of/orangefs/directory/to/be/purged
 *
 *  OR...
 *
 *     # DRY_RUN=1 orangefs-purge /path/of/orangefs/directory/to/be/purged
 *
 * Note, if the dry-run command line flag is not set, using DRY_RUN=0 will purge files!
 *
 * Various results of the purge are logged in the following file:
 *
 *     <log_dir>/<integer_timestamp_of_program_start_time>-<basename of directory argument>.log
 *
 * This allows you to run this program as a cron job and have a separate log file for each execution
 * of the program.
 *
 * Note, log_dir defaults to /var/log/orangefs-purge/.
 *
 * Executing the following command enables the creation of a separate log for multiple "user
 * directories". Note, this will cause one scan to be run per user directory rather than one scan of
 * the overall parent directory:
 *
 *     find /users -mindepth 1 -maxdepth 1 -type d -exec bash -c "orangefs-purge '{}'" \;
 *
 * The above approach is used in the following script: ./scripts/orangefs-purge-user-dirs.sh
 *
 * Note, the parent directory of the log file must exist and be writable. "make install" will
 * attempt to set this up for you!.
 *
 * Any errors encountered are logged to stderr. If running as a cron, you should probably redirect
 * stderr to a file for later reference:
 *
 *     0 3 1 * * orangefs-purge /path/of/orangefs/directory/to/be/purged 2>/var/log/orangefs-purge/orangefs-purge.stderr
 *
 * You may optionally enable logging of files identified for removal by passing the following option
 * to orangefs-purge:
 *
 *     --log-removed-files
 *
 *     NOTE: If you are doing a dry-run, files are not actually removed, yet they will be logged.
 *
 * Output will be a 'R' character followed by a single tab character followed by the absolute path
 * of the file:
 *
 *     R[ tab ]/users/myusername/myfile
 *
 * You may optionally enable logging of files identified for keeping by passing the following option
 * to orangefs-purge:
 *
 *     --log-kept-files
 *
 * Output will be a 'K' character followed by a single tab character followed by the absolute path
 * of the file:
 *
 *     K[ tab ]/users/myusername/myfile
 *
 * -------------------------------------------------------------------------------------------------
 *
 * The goal of this application is to purge files from an OrangeFS file system that have not been
 * "accessed" (read) nor "modified" (written) in the last 31 (30 + 1) days. The +1 is necessary
 * since the recently added relatime feature of OrangeFS is configured to not update the atime more
 * frequently than once per day.
 *
 * Here is the official scratch purge policy as approved by CUCAT: "Any files and directories in
 * scratch space not accessed in the past 30 days will be deleted on the first day of each month."
 *
 * The word access is ambiguous without further clarification. In Linux, access means to read
 * byte(s) from a file and modify means to write byte(s) to a file. It is my understanding that
 * CUCAT meant "not accessed nor modified" regarding the purge policy.
 *
 * Note: The reason directories are not purged is, currently, OrangeFS supports updating
 * atime of files only. Directory timestamps may be updated manually (using "touch" etc.) but there
 * is no automation of this, as there is with file I/O.
 *
 * Note: There is a behavior difference in the Linux relatime vs the relatime feature I implemented
 * for OrangeFS:
 *   - Currently, the atime is potentially updated when a file is accessed according to the normal
 *     relatime semantics.
 *   - The major difference is that the OrangeFS I/O state machine also updates the atime whenever
 *     the file is **modified**.
 *     - This behavior is incorrect and should be fixed in a later release by implementing a sort of
 *       "relmtime" where mtime/ctime updates, caused by writing, cannot occur more than once per
 *       some specified duration such as 24 hours.
 *
 * If you are confused, then you can understand why both the CUCAT policy and the OrangeFS relatime
 * implementation are incorrect; however, they are incorrect in such a way that the "not accessed
 * nor modified" policy may still be implemented using only the OrangeFS atime as a measure of when
 * the file was last accessed or modified. The implementation below also considers mtime when
 * determining if a file should be purged. Currently, mtime is never updated by an OrangeFS write so
 * this has no effect on the outcome of the results, but I went ahead and added it in case the mtime
 * update is added to OrangeFS in the future.
 */
#define _GNU_SOURCE
#define _LARGEFILE_SOURCE 1
#define _LARGEFILE64_SOURCE 1
#include <features.h>

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>

#include "pvfs2.h"
#include <pvfs2-usrint.h>

#define PROGRAM_NAME "orangefs-purge"
#define DEFAULT_LOG_DIR "/var/log/orangefs-purge"
#define DRY_RUN_ENV_VAR     "DRY_RUN"

#define DAY_SECS            (24 * 60 * 60)
#define THIRTYONE_DAYS_SECS (31 * DAY_SECS)

#define PVFS_REQ_LIMIT_DIRENT_COUNT_READDIRPLUS 60

#define LLU(x) ((long long unsigned int) (x))

#if DEBUG_ON == 1
    #define DEBUG(...)                                                        \
    do                                                                        \
    {                                                                         \
        fprintf(stdout, "%s:\t", __func__);                                   \
        fprintf(stdout, ##__VA_ARGS__);                                       \
        fflush(stdout);                                                       \
    } while(0)
#else
    #define DEBUG(...) do {} while(0)
#endif

struct purge_stats_s {
    uint64_t rm_bytes;      /* Bytes successfully removed. */
    uint64_t rm_fils;       /* Files successfully removed. */
    uint64_t frm_bytes;     /* Bytes failed to be removed. */
    uint64_t frm_fils;      /* Files failed to be removed. */
    uint64_t kept_bytes;    /* Bytes not removed. */
    uint64_t kept_fils;     /* Files not removed. */
    uint64_t lnks;          /* Number of symlinks discovered. */
    uint64_t dirs;          /* Number of directories discovered. */
    uint64_t unknown;       /* Number of dirents with unknown type discovered. */
};

typedef enum
{
    LOG_REMOVED_FILES = 256, /* well outside the range of ASCII characters one would use for opts */
    LOG_KEPT_FILES
} long_opts_no_char_type;

struct option const long_opts[] =
{
    {"dry-run", no_argument, NULL, 'd'},
    {"log-dir", required_argument, NULL, 'l'},
    {"log-removed-files", no_argument, NULL, LOG_REMOVED_FILES},
    {"log-kept-files", no_argument, NULL, LOG_KEPT_FILES},
    {"removal-basis-time", required_argument, NULL, 'r'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
};

struct options_s
{
    PVFS_time removal_basis_time;
    char *log_dir;
    int dry_run;
    int log_removed_files;
    int log_kept_files;
};

/* For any of this to work, the system time must be correct and roughly in sync between all the
 * OrangeFS server nodes, OrangeFS clients, and the system from which the purge program will be
 * executed. If this is not the case, then unintended consequences are possible.
 * Both the OrangeFS libraries and this program use the following function to obtain the current
 * time: PINT_util_get_current_time(). This program actually uses get_current_time (see below),
 * which is a clone of PINT_util_get_current_time.
 */

/* GLOBAL VARIABLES */
struct purge_stats_s pstats = {0LL, 0LL, 0LL, 0LL, 0LL, 0LL, 0LL, 0LL, 0LL};
PVFS_credential creds;
PVFS_time removal_basis_time = 0LL;
FILE *logp = NULL;
struct options_s opts;

void orangefs_purge_option_init(struct options_s *x)
{
    x->removal_basis_time = 0LL;
    x->log_dir = NULL;
    x->dry_run = 0;
    x->log_removed_files = 0;
    x->log_kept_files = 0;
}

void usage(int status)
{
    printf("Usage: %s [OPTION]... <ABSOLUTE_PATH_OF_DIRECTORY_TO_BE_PURGED>\n", PROGRAM_NAME);
    printf("\n\
    Walks an OrangeFS directory tree and purges OrangeFS files based on the removal-basis-time.\n\
    If the atime and mtime values of a file are both less than the removal-basis-time then the\n\
    file will be purged.\n\n\
        -h, --help                  show help/usage information.\n\
        -?\n\n\
        -d, --dry-run               does not remove any files but otherwise proceeds as normal.\n\n\
        -l, --log-dir               specify the absolute path of the directory where you want\n\
                                    orangefs-purge to generate its log file. The default is:\n\
                                    /var/log/orangefs-purge/.\n\n\
            --log-kept-files        logs all files that will be kept.\n\n\
            --log-removed-files     logs all files that will be removed.\n\n\
        -r, --removal-basis-time    supply your own removal-basis-time (in seconds since the\n\
                                    UNIX epoch), rather than relying on the default which is 31\n\
                                    days previous to this program's execution time.\n");
    exit(status);
}

/* Log purge statistics */
void log_pstats(FILE *out, struct purge_stats_s *psp)
{
    if(out && psp)
    {
        fprintf(out,
                "removed_bytes\t%llu\n"
                "removed_files\t%llu\n"
                "failed_removed_bytes\t%llu\n"
                "failed_removed_files\t%llu\n"
                "kept_bytes\t%llu\n"
                "kept_files\t%llu\n"
                "directories\t%llu\n"
                "symlinks\t%llu\n"
                "unknown\t%llu\n",
                LLU(psp->rm_bytes),
                LLU(psp->rm_fils),
                LLU(psp->frm_bytes),
                LLU(psp->frm_fils),
                LLU(psp->kept_bytes),
                LLU(psp->kept_fils),
                LLU(psp->dirs),
                LLU(psp->lnks),
                LLU(psp->unknown));
    }
}

/* The following macro function should be used in the ps_* functions below. The macro accepts five
 * arguments:
 *   - psp: a pointer to the purge_stats_s struct
 *   - n:   the numerator of your division calculation
 *   - d:   the denominator of your division calculation
 *   - r:   the return value to use instead of: the result of dividing by zero OR if psp is NULL
 *   - f:   a factor to multiply the result by (useful for calculating percentages)
 */
#define PS_CORRECT_NAN(psp, n, d, r, f)                 \
    do                                                  \
    {                                                   \
        if((psp))                                       \
        {                                               \
            float numerator = (float) (n);              \
            uint64_t denominator = (d);                 \
            if(denominator > 0)                         \
            {                                           \
                return (numerator / denominator) * (f); \
            }                                           \
            else                                        \
            {                                           \
                return (r);                             \
            }                                           \
        }                                               \
                                                        \
        return (r);                                     \
                                                        \
    } while(0)

float ps_percent_bytes_removed(struct purge_stats_s *psp)
{
    PS_CORRECT_NAN(psp,
                   psp->rm_bytes,
                   psp->rm_bytes + psp->frm_bytes + psp->kept_bytes,
                   0.0,
                   100.0);
}

float ps_percent_files_removed(struct purge_stats_s *psp)
{
    PS_CORRECT_NAN(psp,
                   psp->rm_fils,
                   psp->rm_fils + psp->frm_fils + psp->kept_fils,
                   0.0,
                   100.0);
}

float ps_pre_purge_avg_file_size(struct purge_stats_s *psp)
{
    PS_CORRECT_NAN(psp,
                   psp->rm_bytes + psp->frm_bytes + psp->kept_bytes,
                   psp->rm_fils + psp->frm_fils + psp->kept_fils,
                   0.0,
                   1.0);
}

float ps_post_purge_avg_file_size(struct purge_stats_s *psp)
{
    PS_CORRECT_NAN(psp,
                   psp->frm_bytes + psp->kept_bytes,
                   psp->frm_fils + psp->kept_fils,
                   0.0,
                   1.0);
}

float ps_purged_avg_file_size(struct purge_stats_s *psp)
{
    PS_CORRECT_NAN(psp,
                   psp->rm_bytes,
                   psp->rm_fils,
                   0.0,
                   1.0);
}

void log_pstats_more(FILE *out, struct purge_stats_s *psp)
{
    if(out && psp)
    {
        fprintf(out,
                "percent_bytes_removed\t%f\n"
                "percent_files_removed\t%f\n"
                "pre_purge_avg_file_size\t%f\n"
                "post_purge_avg_file_size\t%f\n"
                "purged_avg_file_size\t%f\n",
                ps_percent_bytes_removed(psp),
                ps_percent_files_removed(psp),
                ps_pre_purge_avg_file_size(psp),
                ps_post_purge_avg_file_size(psp),
                ps_purged_avg_file_size(psp));
    }
}

/*
 * Returns time in seconds since Epoch (see time(2)).
 * This is an exact clone of PINT_util_get_current_time() function of OrangeFS for compatibility.
 */
PVFS_time get_current_time(void)
{
    struct timeval t = {0,0};
    PVFS_time current_time = 0;

    gettimeofday(&t, NULL);
    current_time = (PVFS_time)t.tv_sec;
    return current_time;
}

/* Converts the supplied PVFS_time to a human readable string format. The returned string should be
 * freed when you are finished with it! */
char *human_readable_time(PVFS_time t)
{
    char *human_time = NULL;
    char *ret = NULL;
    time_t t1 = (time_t) t;
    human_time = ctime(&t1);
    if(human_time)
    {
        ret = strdup(human_time);
    }
    return ret;
}

/* A clone of an internal OrangeFS convenience function for converting PVFS_sys_attr attributes to
 * normal struct stat attributes. */
int sys_attr_to_stat(struct stat *bufp, PVFS_sys_attr *attrp, PVFS_object_ref ref)
{
    if(!(bufp && attrp) ||
        (ref.handle == PVFS_HANDLE_NULL || ref.fs_id == PVFS_FS_ID_NULL))
    {
        return -1;
    }

    /* We adapt iocommon_stat to convert PVFS_sys_attr to regular struct stat */
    memset(bufp, 0, sizeof(struct stat));

    /* copy attributes into standard stat struct */
    bufp->st_dev = ref.fs_id;
    bufp->st_ino = ref.handle;
    bufp->st_mode = attrp->perms;
    if (attrp->objtype == PVFS_TYPE_METAFILE)
    {
        bufp->st_mode |= S_IFREG;
        bufp->st_nlink = 1; /* PVFS does not allow hard links */
    }
    if (attrp->objtype == PVFS_TYPE_DIRECTORY)
    {
        bufp->st_mode |= S_IFDIR;
        bufp->st_nlink = attrp->dirent_count + 2;
    }
    if (attrp->objtype == PVFS_TYPE_SYMLINK)
    {
        bufp->st_mode |= S_IFLNK;
        bufp->st_nlink = 1; /* PVFS does not allow hard links */
    }
    bufp->st_uid = attrp->owner;
    bufp->st_gid = attrp->group;
    bufp->st_rdev = 0; /* no dev special files */
    bufp->st_size = attrp->size;
#if PVFS_USER_ENV_VARS_ENABLED
    if(env_vars.env_var_array[ORANGEFS_STRIP_SIZE_AS_BLKSIZE].env_var_value &&
       strcmp(env_vars.env_var_array[ORANGEFS_STRIP_SIZE_AS_BLKSIZE].env_var_value, "true") == 0)
    {
        if(attrp->dfile_count > 0)
        {
            bufp->st_blksize = attrp->blksize / attrp->dfile_count;
        }
        else
        {
            bufp->st_blksize = attrp->blksize;
        }
    }
    else
    {
        bufp->st_blksize = attrp->blksize;
    }
#else
    bufp->st_blksize = attrp->blksize;
#endif
    bufp->st_blocks = (attrp->size + (S_BLKSIZE - 1)) / S_BLKSIZE;
    /* we don't have nsec so we left the memset zero them */
    bufp->st_atime = attrp->atime;
    bufp->st_mtime = attrp->mtime;
    bufp->st_ctime = attrp->ctime;

    return 0;
}

/* Walks an OrangeFS directory tree using a recursive algorithm and the PVFS_sys_readdirplus
 * function which is the most efficient way to gather stats from multiple entries at once when using
 * OrangeFS.
 */
int walk_rdp_and_purge(char *path, PVFS_object_ref *dir_refp)
{
    /* This is a recursive algorithm so I try not to declare too many local variables and
     * parameters to avoid a stack overflow. My development machine stack limit is 8 MB:
     *
     * $ ulimit -s
     * 8192
     *
     */
    PVFS_sysresp_readdirplus rdplus_response;       /* 48 B */
    PVFS_object_ref dirent_ref;                     /* 16 B */
    char * dirent_path;                             /*  8 B */
    PVFS_ds_position token = PVFS_READDIR_START;    /*  8 B */
    uint64_t entry_count = 0LL;                     /*  8 B */
    int ret = 0;                                    /*  4 B */
    short dir_len = 0;                              /*  2 B */

    if(!path || !dir_refp)
    {
        fprintf(stderr,
                "%s: ERROR: invalid arguments passed to the walk_rdp_and_purge function: "
                "path = %s\n",
                __func__,
                path);
        return -1;
    }

    /* Allocate space on the heap for storing dirents of the parent directory */
    dir_len = strlen(path);
    dirent_path = (char *) calloc(1, PVFS_PATH_MAX);
    strncpy(dirent_path, path, PVFS_PATH_MAX - 1);

    DEBUG("INFO: scanning with rdp, path = %s\n", path);

    /* Set up the fs_id to be used later when converting from PVFS_sys_attr to struct stat. */
    dirent_ref.fs_id = dir_refp->fs_id;

    while(1)
    {
        int i = 0;

        memset(&rdplus_response, 0, sizeof(PVFS_sysresp_readdirplus));
        ret = PVFS_sys_readdirplus(*dir_refp,
                                token,
                                PVFS_REQ_LIMIT_DIRENT_COUNT_READDIRPLUS,
                                &creds, 
                                PVFS_ATTR_SYS_ALL_NOHINT,
                                &rdplus_response,
                                NULL);

        if(ret < 0)
        {
            fprintf(stderr,
                    "%s: ERROR: PVFS_sys_readdirplus failed with ret= %d\n",
                    __func__,
                    ret);
            ret = -1;
            goto cleanup;
        }

        entry_count += rdplus_response.pvfs_dirent_outcount;

        if(rdplus_response.pvfs_dirent_outcount)
        {
            for(i = 0; i < rdplus_response.pvfs_dirent_outcount; i++)
            {
                struct stat buf;

                DEBUG("INFO: rdplus_response.dirent_array[%u].d_name = %s\n",
                      i,
                      rdplus_response.dirent_array[i].d_name);
                DEBUG("INFO: rdplus_response.attr_array[%u].size = %llu\n",
                      i,
                      LLU(rdplus_response.attr_array[i].size));

                /* Must fill in the handle from the dirent prior to converting attributes from
                 * PVFS_sys_attr to stat. */
                dirent_ref.handle = rdplus_response.dirent_array[i].handle;
                ret = sys_attr_to_stat(&buf, &rdplus_response.attr_array[i], dirent_ref);
                if(ret < 0)
                {
                    fprintf(stderr,
                            "%s: ERROR: sys_attr_to_stat failed with ret= %d\n",
                            __func__,
                            ret);
                    ret = -1;
                    goto cleanup;
                }

                /* **ALWAYS** zero the bytes after the parent directory. */
                memset(&dirent_path[dir_len], 0, PVFS_PATH_MAX - dir_len);
                DEBUG("INFO: dirent_path = %s\n",
                      dirent_path);
                dirent_path[dir_len] = '/';

                /* Copy starting after the '/' character. */
                strncpy(&dirent_path[dir_len + 1],
                        rdplus_response.dirent_array[i].d_name,
                        PATH_MAX - 1 - dir_len - 1);

                DEBUG("INFO: dirent_path = %s\n", dirent_path);

                /* file/link/dir? */
                if(S_ISREG(buf.st_mode))
                {
                    DEBUG("\t\tFILE\n");

                    if(buf.st_atime < (time_t) removal_basis_time &&
                       buf.st_mtime < (time_t) removal_basis_time)
                    {
                        if(opts.log_removed_files)
                        {
                            fprintf(logp, "R\t%s\n", dirent_path);
                        }

                        if(!opts.dry_run)
                        {
                            ret = PVFS_sys_remove(rdplus_response.dirent_array[i].d_name,
                                                *dir_refp,
                                                &creds,
                                                NULL);
                            if(ret < 0)
                            {
                                pstats.frm_fils++;
                                pstats.frm_bytes += buf.st_size;
                                PVFS_perror("PVFS_sys_remove", ret);
                                fprintf(stderr,
                                        "%s: WARNING: failed to remove path = %s\n",
                                        __func__,
                                        dirent_path);
                                PVFS_util_release_sys_attr(&rdplus_response.attr_array[i]);
                                continue;
                            }
                        }

                        pstats.rm_fils++;
                        pstats.rm_bytes += buf.st_size;

                    }
                    else
                    {
                        if(opts.log_kept_files)
                        {
                            fprintf(logp, "K\t%s\n", dirent_path);
                        }

                        pstats.kept_fils++;
                        pstats.kept_bytes += buf.st_size;
                    }

#if DEBUG_ON == 1
                    char *readable_time = NULL;
                    DEBUG("INFO: atime was %llu or %s",
                          LLU(buf.st_atime),
                          (readable_time = human_readable_time((PVFS_time) buf.st_atime)));
                    free(readable_time);
#endif

                }
                else if(S_ISDIR(buf.st_mode))
                {
                    DEBUG("\t\tDIR\n");
                    pstats.dirs++;
                    /* Recurse! */
                    ret = walk_rdp_and_purge(dirent_path, &dirent_ref);
                    if(ret != 0)
                    {
                        PVFS_util_release_sys_attr(&rdplus_response.attr_array[i]);
                        return -1;
                    }
                }
                else if(S_ISLNK(buf.st_mode))
                {
                    DEBUG("\t\tLNK\n");
                    pstats.lnks++;
                }
                else
                {
                    fprintf(stderr,
                            "%s: ERROR: UNRECOGNIZED DIRENT TYPE at path: %s\n",
                            __func__,
                            dirent_path);
                    pstats.unknown++;
                }

                PVFS_util_release_sys_attr(&rdplus_response.attr_array[i]);

            } /* Done iterating over gathered entries and stats */

            /* TODO Why must I do this pointer arithmetic below?!
             * Is there a problem with PINT_MALLOC?
             * Why can't I just free! */
#if USING_PINT_MALLOC == 1
            free((char *) (rdplus_response.dirent_array) - 32);
            free((char *) (rdplus_response.stat_err_array) - 32);
            free((char *) (rdplus_response.attr_array) - 32);
#else
            free(rdplus_response.dirent_array);
            free(rdplus_response.stat_err_array);
            free(rdplus_response.attr_array);
#endif

        } /* Check for more dirents via PVFS_sys_readdirplus! */

        if(rdplus_response.token == PVFS_ITERATE_END)
        {
            break;
        }

        token = rdplus_response.token;

    } /* END while(1) */

cleanup:
    free(dirent_path);
    DEBUG("INFO: entry_count = %llu\n",
          LLU(entry_count));
    return ret;
}


/* This program accepts options defined above and following them **one** directory argugument, the
 * absolute path of the directory tree to be walked for purging of expired files. */
int main(int argc, char **argv)
{
    PVFS_time current_time = 0LL;
    PVFS_time finish_time = 0LL;
    PVFS_time creds_timeout = 0LL;
    char *current_time_str = NULL;
    char *removal_basis_time_str = NULL;
    char *finish_time_str = NULL;
    char *dir = NULL;
    char *dry_run_str = NULL;
    char log_path[PATH_MAX] = { 0 };
    char resolved_path[PVFS_PATH_MAX] = { 0 };
    struct stat arg_stat;
    PVFS_object_ref dir_ref;
    PVFS_sysresp_lookup lk_response;
    int ret;
    int c;
    PVFS_fs_id fs_id;

    if(geteuid() != 0)
    {
        fprintf(stderr, "ERROR: This program must be run as root.\n");
        return -1;
    }

    orangefs_purge_option_init(&opts);

    while((c = getopt_long(argc, argv, "?dhl:r:", long_opts, NULL)) != -1)
    {
        switch (c)
        {
            case 'd':
                opts.dry_run = 1;
                break;
            case 'l':
                opts.log_dir = strdup(optarg);
                break;
            case LOG_REMOVED_FILES:
                opts.log_removed_files = 1;
                break;
            case LOG_KEPT_FILES:
                opts.log_kept_files = 1;
                break;
            case 'r':
                opts.removal_basis_time = strtoull(optarg, NULL, 0);
                break;
            case '?':
            case 'h':
                usage(EXIT_SUCCESS);
                break;
            default:
                usage(EXIT_FAILURE);
        }
    }

    dir = argv[optind];
    if(dir == NULL)
    {
        usage(EXIT_FAILURE);
    }

    /* Dry Run? */
    dry_run_str = getenv(DRY_RUN_ENV_VAR);
    if(dry_run_str)
    {
        /* Use a local variable here instead of opts.dry_run so that if the environment variable is
         * set to false, it cannot override the dry-run command line flag if it is used. */
        int dr = atoi(dry_run_str);

        if(dr != 0)
        {
            opts.dry_run = 1;
        }
    }

    ret = PVFS_util_init_defaults();
    if(ret < 0)
    {
        PVFS_perror("PVFS_util_init_defaults", ret);
        return -1;
    }

    current_time = get_current_time();

    /* Generate a credential with a **long** timeout so that we don't have to worry
     * about refreshing the credential and paying a latency penalty for doing so. Doing the
     * following resolves the errors that would be generated by long running programs that don't
     * make any attempt to refresh the credential.
     */
#if USE_DEFAULT_CREDENTIAL_TIMEOUT == 0
    /* Set the credential timeout for 30 days in the future. */
    creds_timeout = 30 * DAY_SECS; /* A TO other than zero will create a TO of (now + timeout). */
#else
    creds_timeout = 0; /* A TO of 0 will cause the following function to use the default TO. */
#endif
    ret= PVFS_util_gen_credential(NULL,
                                  NULL,
                                  creds_timeout,
                                  NULL,
                                  NULL,
                                  &creds);
    if (ret < 0)
    {
        PVFS_perror("PVFS_util_gen_credential", ret);
        ret = -1;
        goto cleanup_cred;
    }

    DEBUG("INFO: Credential timeout is %llu\n", LLU(creds.timeout));
    DEBUG("INFO: Credential timeout is %f days in the future.\n",
          ((float)(creds.timeout - current_time) / DAY_SECS));

    if((ret = lstat(dir, &arg_stat)))
    {
        perror("ERROR: Could not stat path supplied as the first argument, reason= ");
        ret = -1;
        goto cleanup_cred;
    }

    if((arg_stat.st_mode & S_IFMT) != S_IFDIR)
    {
        fprintf(stderr,
                "ERROR: supplied argument is a valid path but not a directory! path = %s\n",
                dir);
        ret = -1;
        goto cleanup_cred;
    }

    ret = PVFS_util_resolve(dir, 
                            &fs_id, 
                            resolved_path, 
                            PVFS_PATH_MAX);

    if (ret < 0)
    {
        fprintf(stderr,
                "%s: ERROR: PVFS_util_resolve failed, could not find"
                " file system for %s\n",
                __func__,
                dir);
        ret = -1;
        goto cleanup_cred;
    }

    /* Resolved path does not include the OrangeFS mount prefix e.g. /mnt/orangefs */
    DEBUG("INFO: PVFS path resolved. fs_id = %d, resolved_path = %s\n",
          fs_id,
          resolved_path);

    if(strlen(resolved_path) == 0)
    {
        DEBUG("INFO: Detected a resolved path of length == 0. "
              "Continuing assuming the OrangeFS '/' path was the intended target.\n");
        resolved_path[0] = '/';
        resolved_path[1] = 0;
    }

    /* What directory are we scanning? */
    ret = PVFS_sys_lookup(fs_id,
                          resolved_path,
                          &creds,
                          &lk_response,
                          PVFS2_LOOKUP_LINK_NO_FOLLOW,
                          NULL);
    if(ret < 0)
    {
        PVFS_perror("ERROR: PVFS_sys_lookup", ret);
        ret = -1;
        goto cleanup_cred;
    }

    dir_ref.handle = lk_response.ref.handle;
    dir_ref.fs_id = fs_id;

    DEBUG("INFO: dir_ref.handle = %llu, dir_ref.fs_id = %d\n",
          LLU(dir_ref.handle),
          dir_ref.fs_id);

    /* Convert time to human readable string format. */
    current_time_str = human_readable_time(current_time);

    /* NOTE Files with atime and mtime less than removal_basis_time will be removed. */

    /* Since a timestamp of 0 predates OrangeFS, for this program, it is safe to assume the
     * removal_basis_time was not configured by the user or the user is indicating they wan't to
     * use the default removal policy of 31 days prior to this program's execution start time as
     * calculated below. */
    if(opts.removal_basis_time == 0)
    {
        removal_basis_time = current_time - THIRTYONE_DAYS_SECS;
    }
    else
    {
        removal_basis_time = opts.removal_basis_time;
    }

    removal_basis_time_str = human_readable_time(removal_basis_time);

    /* determine basename of supplied path and embed it in the log file name. */
    snprintf(log_path,
             PATH_MAX,
             "%s/%llu-%s.log",
             opts.log_dir ? opts.log_dir : DEFAULT_LOG_DIR,
             LLU(current_time),
             basename(dir));
    DEBUG("INFO: log_path\t%s\n", log_path);
    logp = fopen(log_path, "w");
    if(!logp)
    {
        fprintf(stderr,
                "ERROR: Couldn't open orangefs-purge log. Now logging to stderr!\n");
        logp = stderr;
    }

    fprintf(logp, "directory\t%s\n", dir);
    fprintf(logp, "dry_run\t%s\n", opts.dry_run == 0 ? "false" : "true");
    fprintf(logp, "current_time\t%llu\n", LLU(current_time));
    fprintf(logp, "current_time_str\t%s", current_time_str);
    fprintf(logp, "removal_basis_time\t%llu\n", LLU(removal_basis_time));
    fprintf(logp, "removal_basis_time_str\t%s", removal_basis_time_str);
    free(current_time_str);
    free(removal_basis_time_str);

    ret = walk_rdp_and_purge(dir, &dir_ref);

    finish_time = get_current_time();
    finish_time_str = human_readable_time(finish_time);
    fprintf(logp, "finish_time\t%llu\n", LLU(finish_time));
    fprintf(logp, "finish_time_str\t%s", finish_time_str);
    free(finish_time_str);

    fprintf(logp, "duration_seconds\t%llu\n", LLU(finish_time - current_time));
    log_pstats(logp, &pstats);
    log_pstats_more(logp, &pstats);

cleanup_cred:
    /* NOTE It would be nice to have a cleanup function for apps generating their own creds e.g.
     * PINT_cleanup_credential(&creds); */

    free(opts.log_dir);

    if(ret == 0)
    {
        fprintf(logp, "purge_success\ttrue\n");
        fclose(logp);
        return 0;
    }
    else
    {
        fprintf(logp, "purge_success\tfalse\n");
        fclose(logp);
        return 1;
    }
}
