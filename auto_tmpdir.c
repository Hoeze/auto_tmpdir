/*
 * auto_tmpdir
 *
 * SLURM SPANK plugin that automates the process of creating/destroying
 * temporary directories for jobs/steps.
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

#include <slurm/spank.h>
#include <slurm/slurm.h>

#define XSTR(A) STR(A)
#define STR(A) #A

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(auto_tmpdir, 1)

/*
 * Default TMPDIR prefix:
 */
static const char *default_tmpdir_prefix = "/tmp";

/*
 * Shared TMPDIR prefix:
 */
#ifdef WITH_SHARED_STORAGE
static const char *shared_tmpdir_prefix = XSTR(SHARED_STORAGE_PATH);
#endif

/*
 * Directory format strings:
 */
static const char *job_dir_sprintf_format = "%s/job_%u";
static const char *job_step_dir_sprintf_format = "%s/job_%u/step_%u.%u";
#ifdef WITH_SHARED_STORAGE
static const char *pernode_job_dir_sprintf_format = "%s/job_%u/%s";
static const char *pernode_job_step_dir_sprintf_format = "%s/job_%u/%s/step_%u.%u";
#endif


/*
 * What's the base directory to use for temp files?
 */
static char *base_tmpdir = NULL;

/*
 * @function _opt_tmpdir
 *
 * Parse the --tmpdir=<path> option.
 *
 */
static int _opt_tmpdir(
    int         val,
    const char  *optarg,
    int         remote
)
{
    if ( *optarg != '/' ) {
        slurm_error("auto_tmpdir:  invalid path to --tmpdir: %s", optarg);
        return ESPANK_BAD_ARG;
    }

    if ( base_tmpdir ) free(base_tmpdir);
    base_tmpdir = strdup(optarg);
    slurm_verbose("auto_tmpdir:  temporary directories under %s", base_tmpdir);
    return ESPANK_SUCCESS;
}


/*
 * Should we remove temp directories we create?
 */
static int should_remove_tmpdir = 1;

/*
 * @function _opt_no_rm_tmpdir
 *
 * Parse the --no-rm-tmpdir option.
 *
 */
static int _opt_no_rm_tmpdir(
    int         val,
    const char  *optarg,
    int         remote
)
{
    should_remove_tmpdir = 0;
    slurm_verbose("auto_tmpdir:  will not remove tempororary directories");
    return ESPANK_SUCCESS;
}


/*
 * Should we create per-step sub-directories?
 */
static int should_create_per_step_tmpdirs = 1;

/*
 * @function _opt_no_step_tmpdir
 *
 * Parse the --no-step-tmpdir option.
 *
 */
static int _opt_no_step_tmpdir(
    int         val,
    const char  *optarg,
    int         remote
)
{
    should_create_per_step_tmpdirs = 0;
    slurm_verbose("auto_tmpdir:  will not create per-step tempororary directories");
    return ESPANK_SUCCESS;
}

#ifdef WITH_SHARED_STORAGE
/*
 * Place the tmpdir on shared storage? (overridden by --tmpdir)
 */
static int should_create_on_shared = 0;
static int should_add_pernode_on_shared = 0;

/*
 * @function _opt_use_shared_tmpdir
 *
 * Parse the --use-shared-tmpdir option.
 *
 */
static int _opt_use_shared_tmpdir(
    int         val,
    const char  *optarg,
    int         remote
)
{
    /*
     * Check the optarg to see if "per-node" is being requested:
     */
    if ( optarg && strcmp(optarg, "(null)") ) {
        if ( strcmp(optarg, "per-node") == 0 ) {
            should_add_pernode_on_shared = 1;
        } else {
            slurm_error("auto_tmpdir:  invalid --use-shared-tmpdir optional value: %s", optarg);
            return ESPANK_BAD_ARG;
        }
    }

    should_create_on_shared = 1;
    slurm_verbose("auto_tmpdir:  will use shared tempororary directory on %s", shared_tmpdir_prefix);

    return ESPANK_SUCCESS;
}
#endif

/*
 * Options available to this spank plugin:
 */
struct spank_option spank_options[] =
    {
        { "tmpdir", "<path>",
            "Use the given path as the base directory for temporary files.",
            1, 0, (spank_opt_cb_f) _opt_tmpdir },

        { "no-step-tmpdir", NULL,
            "Do not create per-step sub-directories.",
            0, 0, (spank_opt_cb_f) _opt_no_step_tmpdir },

        { "no-rm-tmpdir", NULL,
            "Do not automatically remove temporary directories for the job/steps.",
            0, 0, (spank_opt_cb_f) _opt_no_rm_tmpdir },

#ifdef WITH_SHARED_STORAGE
        { "use-shared-tmpdir", NULL,
            "Create temporary directories on shared storage (overridden by --tmpdir).  Use \"--use-shared-tmpdir=per-node\" to create unique sub-directories for each node allocated to the job (e.g. <base>/job_<jobid>/<nodename>).",
            2, 0, (spank_opt_cb_f) _opt_use_shared_tmpdir },
#endif

        SPANK_OPTIONS_TABLE_END
    };


/**/


/*
 * @function _get_base_tmpdir
 *
 * Returns the configured base directory for temporary directories
 * or /tmp by default.
 *
 * Privileges must have been dropped prior to this function's being called.
 *
 */
const char*
_get_base_tmpdir()
{
    const char      *path = NULL;
    int             had_initial_error = 0;

#ifdef WITH_SHARED_STORAGE
    if ( base_tmpdir ) {
        path = base_tmpdir;
        slurm_debug("auto_tmpdir(00): chose base tmpdir %s", path);
    }
    else if ( should_create_on_shared ) {
        path = shared_tmpdir_prefix;
        slurm_debug("auto_tmpdir(01): chose base tmpdir %s", path);
    }
    else {
        path = default_tmpdir_prefix;
        slurm_debug("auto_tmpdir(02): chose base tmpdir %s", path);
    }
#else
    if ( base_tmpdir ) {
        path = base_tmpdir;
        slurm_debug("auto_tmpdir(03): chose base tmpdir %s", path);
    }
    else {
        path = default_tmpdir_prefix;
        slurm_debug("auto_tmpdir(04): chose base tmpdir %s", path);
    }
#endif

retry_test:
    if ( access(path, R_OK|W_OK|X_OK) == 0 ) {
        if ( had_initial_error ) slurm_error("auto_tmpdir: defaulting to temporary directory base path: %s", path);
        slurm_debug("auto_tmpdir: final base tmpdir %s", path);
        return path;
    }
    slurm_error("auto_tmpdir: no access to temporary directory base path (errno = %d): %s", errno, path);

    if ( path == base_tmpdir ) {
#ifdef WITH_SHARED_STORAGE
        if ( should_create_on_shared ) {
            path = shared_tmpdir_prefix;
        } else {
            path = default_tmpdir_prefix;
        }
#else
        path = default_tmpdir_prefix;
#endif
        had_initial_error = 1;
        goto retry_test;
    }
#ifdef WITH_SHARED_STORAGE
    else if ( path == shared_tmpdir_prefix ) {
        path = default_tmpdir_prefix;
        had_initial_error = 1;
        goto retry_test;
    }
#endif

    return NULL;
}

/*
 * @function _sprint_tmpdir
 *
 * Fill a character buffer with the tmpdir name to be used.
 *
 * Privileges must have been dropped prior to this function's being called.
 *
 */
int
_sprint_tmpdir(
    char            *buffer,
    size_t          buffer_capacity,
    uint32_t        job_id,
    uint32_t        job_step_id,
    uint32_t        local_task_id,
    int             ignore_pernode,
    const char*     *tmpdir_prefix
)
{
    size_t          actual_len;
    const char      *tmpdir = _get_base_tmpdir();

    if ( tmpdir ) {
#ifdef WITH_SHARED_STORAGE
        if ( (tmpdir == shared_tmpdir_prefix) && should_add_pernode_on_shared ) {
            char    hostname[64];
            int     i;

            /* 64 characters should be plenty -- DNS label length maxes at 63 */
            gethostname(hostname, sizeof(hostname));
            /* Find the first dot (.) and NUL-terminate there (or at the end of the buffer): */
            i = 0;
            while ( (i < (sizeof(hostname) - 1)) && hostname[i] && (hostname[i] != '.') ) i++;
            hostname[i] = '\0';

            if ( ! should_create_per_step_tmpdirs || ((job_step_id == SLURM_BATCH_SCRIPT) || (job_step_id == SLURM_EXTERN_CONT)) ) {
                if ( ignore_pernode ) {
                    actual_len = snprintf(buffer, buffer_capacity, job_dir_sprintf_format, tmpdir, job_id);
                } else {
                    actual_len = snprintf(buffer, buffer_capacity, pernode_job_dir_sprintf_format, tmpdir, job_id, hostname);
                }
            } else {
                actual_len = snprintf(buffer, buffer_capacity, pernode_job_step_dir_sprintf_format, tmpdir, job_id, hostname, job_step_id, local_task_id);
            }
        }
        else
#endif
        if ( ! should_create_per_step_tmpdirs || ((job_step_id == SLURM_BATCH_SCRIPT) || (job_step_id == SLURM_EXTERN_CONT)) ) {
            actual_len = snprintf(buffer, buffer_capacity, job_dir_sprintf_format, tmpdir, job_id);
        } else {
            actual_len = snprintf(buffer, buffer_capacity, job_step_dir_sprintf_format, tmpdir, job_id, job_step_id, local_task_id);
        }
        if ( (actual_len > 0) && (actual_len < buffer_capacity) ) {
            if ( tmpdir_prefix ) *tmpdir_prefix = tmpdir;
            return actual_len;
        }
    }
    return -1;
}

/*
 * @function _mktmpdir
 *
 * Given a job id and step id, create the temporary directory.
 *
 * Privileges must have been dropped prior to this function's being called.
 *
 */
int
_mktmpdir(
    char            *outTmpDir,
    size_t          outTmpDirLen,
    uint32_t        job_id,
    uint32_t        job_step_id,
    uint32_t        local_task_id
)
{
    const char      *tmpdir = NULL;
    int             actual_len = 0;

    /* Decide which format the directory should use and determine string length: */
    actual_len = _sprint_tmpdir(outTmpDir, outTmpDirLen, job_id, job_step_id, local_task_id, 0, &tmpdir);
    if ( ! tmpdir ) return (-1);

    /* If that failed then we've got big problems: */
    if ( (actual_len < 0) || (actual_len >= outTmpDirLen) ) {
        slurm_error("auto_tmpdir: Failure while creating new tmpdir path: %d >= %d", actual_len, outTmpDirLen);
        return (-1);
    } else {
        struct stat     finfo;
        int             stat_rc;
        int             i;

        /* _get_base_tmpdir guarantees that the tmpdir exists.  So we need to start at
         * there:
         */
        i = strlen(tmpdir);
        while ( outTmpDir[i] == '/' ) {
            /* Yep, a directory under the base tmpdir */
            outTmpDir[i] = '\0';
            if ( stat(outTmpDir, &finfo) == 0 ) {
                /* Ensure it's a directory: */
                if ( ! S_ISDIR(finfo.st_mode) ) {
                    slurm_error("auto_tmpdir: tmpdir component is not a directory: %s", outTmpDir);
                    return (-1);
                }
            }
            else if ( mkdir(outTmpDir, 0700) != 0 ) {
                if ( errno != EEXIST ) {
                    slurm_error("auto_tmpdir: unable to create tmpdir component %s (errno = %d)", outTmpDir, errno);
                    return (-1);
                }
            }
            /* Okay, that component exists.  Move to the next one: */
            outTmpDir[i++] = '/';
            while ( (i < outTmpDirLen) && (outTmpDir[i] != '/') ) i++;
        }

        /* Now ensure that final directory exists: */
        if ( stat(outTmpDir, &finfo) == 0 ) {
            /* Ensure it's a directory: */
            if ( ! S_ISDIR(finfo.st_mode) ) {
                slurm_error("auto_tmpdir: tmpdir component is not a directory: %s", outTmpDir);
                return (-1);
            }
        }
        else if ( mkdir(outTmpDir, 0700) != 0 ) {
            slurm_error("auto_tmpdir: unable to create tmpdir component %s (errno = %d)", outTmpDir, errno);
            return (-1);
        }
    }
    return actual_len;
}


/*
 * @function _rmdir_recurse
 *
 * Recursively remove a file path.
 *
 * Privileges must have been dropped prior to this function's being called.
 *
 */
int
_rmdir_recurse(
    const char      *path
)
{
    int             rc = 0;

    char            *path_argv[2] = { (char*)path, NULL };

    // FTS_NOCHDIR  - Avoid changing cwd, which could cause unexpected behavior
    //                in multithreaded programs
    // FTS_PHYSICAL - Don't follow symlinks. Prevents deletion of files outside
    //                of the specified directory
    // FTS_XDEV     - Don't cross filesystem boundaries
    FTS             *ftsPtr = fts_open(path_argv, FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, NULL);
    FTSENT          *ftsItem;

    if ( ! ftsPtr ) {
        slurm_info("auto_tmpdir: _rmdir_recurse(): Failed to open file traversal context on %s: %s", path, strerror(errno));
        return (-1);
    }

    //
    // Read the room item -- should be a directory owned by ownerUid:
    //
    if ( (ftsItem = fts_read(ftsPtr)) ) {
        switch ( ftsItem->fts_info ) {
            case FTS_NS:
            case FTS_DNR:
            case FTS_ERR: {
                slurm_info("auto_tmpdir: _rmdir_recurse(%s): directory does not exist", path);
                break;
            }
            case FTS_D: {
                //
                // We're entering a directory -- exactly what we want!
                //
                while ( (ftsItem = fts_read(ftsPtr)) ) {
                    switch ( ftsItem->fts_info ) {
                        case FTS_NS:
                        case FTS_DNR:
                        case FTS_ERR:
                            slurm_info("auto_tmpdir: _rmdir_recurse(): Error in fts_read(%s): %s\n", ftsItem->fts_accpath, strerror(ftsItem->fts_errno));
                            rc = -1;
                            break;

                        case FTS_DC:
                        case FTS_DOT:
                        case FTS_NSOK:
                            // Not reached unless FTS_LOGICAL, FTS_SEEDOT, or FTS_NOSTAT were
                            // passed to fts_open()
                            break;

                        case FTS_D:
                            // Do nothing. Need depth-first search, so directories are deleted
                            // in FTS_DP
                            break;

                        case FTS_DP:
                        case FTS_F:
                        case FTS_SL:
                        case FTS_SLNONE:
                        case FTS_DEFAULT:
                            if ( remove(ftsItem->fts_accpath) < 0 ) {
                                slurm_info("auto_tmpdir: _rmdir_recurse(): Failed to remove %s: %s\n", ftsItem->fts_path, strerror(errno));
                                rc = -1;
                            }
                            break;
                    }
                }
                break;
            }
        }
    }
    fts_close(ftsPtr);
    return rc;
}


/*
 * @function slurm_spank_init
 *
 * In the ALLOCATOR context, the 'spank_options' don't get automatically
 * registered as they do under LOCAL and REMOTE.  So under that context
 * we explicitly register our cli options.
 *
 * In the REMOTE context, go ahead and create the temp directory and
 * assign appropriate ownership.
 *
 */
int
slurm_spank_init(
    spank_t       spank_ctxt,
    int           argc,
    char          *argv[]
)
{
    int                     rc = ESPANK_SUCCESS;
    int                     i;

    switch ( spank_context() ) {

        case S_CTX_ALLOCATOR: {
            struct spank_option   *o = spank_options;

            while ( o->name && (rc == ESPANK_SUCCESS) ) rc = spank_option_register(spank_ctxt, o++);
            break;
        }

        case S_CTX_REMOTE: {
            char            v[PATH_MAX];

            //
            // Check for our arguments in the environment:
            //
            if ( spank_getenv(spank_ctxt, "SLURM_SPANK__SLURM_SPANK_OPTION_auto_tmpdir_no_rm_tmpdir", v, sizeof(v)) == ESPANK_SUCCESS ) {
                _opt_no_rm_tmpdir(0, v, 1);
            }
            if ( spank_getenv(spank_ctxt, "SLURM_SPANK__SLURM_SPANK_OPTION_auto_tmpdir_no_step_tmpdir", v, sizeof(v)) == ESPANK_SUCCESS ) {
                _opt_no_step_tmpdir(0, v, 1);
            }
            if ( spank_getenv(spank_ctxt, "SLURM_SPANK__SLURM_SPANK_OPTION_auto_tmpdir_tmpdir", v, sizeof(v)) == ESPANK_SUCCESS ) {
                _opt_tmpdir(0, v, 1);
            }
            if ( spank_getenv(spank_ctxt, "SLURM_SPANK__SLURM_SPANK_OPTION_auto_tmpdir_use_shared_tmpdir", v, sizeof(v)) == ESPANK_SUCCESS ) {
                _opt_use_shared_tmpdir(0, v, 1);
            }
            break;
        }

    }
    return rc;
}


/*
 * @function slurm_spank_task_init
 *
 * Set job-specific TMPDIR in environment.  For batch scripts the path
 * uses just the job id; for all others, the path uses the job id, a dot,
 * and the job step id.  The value of TMPDIR handed to us by SLURM is
 * the base path for the new TMPDIR; if SLURM doesn't hand us a TMPDIR
 * then we default to using /tmp as our base directory.
 *
 * This function does not actually create the directory.
 *
 * (Called from slurmstepd after it starts.)
 */
int
slurm_spank_task_init(
    spank_t       spank_ctxt,
    int           argc,
    char          *argv[]
)
{
    int           rc = ESPANK_SUCCESS;
    char          tmpdir[PATH_MAX];
    int           tmpdirlen = 0;
    uint32_t      job_id, job_step_id, task_id;
    uid_t         jobUid = -1, savedUid = geteuid();
    gid_t         jobGid = -1, savedGid = getegid();
    int           didSetUid = 0, didSetGid = 0;

    /* Get the job id and step id: */
    if ( (rc = spank_get_item(spank_ctxt, S_JOB_ID, &job_id)) != ESPANK_SUCCESS ) {
        slurm_error("auto_tmpdir: no job id associated with job??");
        return rc;
    }
    if ( (rc = spank_get_item(spank_ctxt, S_JOB_STEPID, &job_step_id)) != ESPANK_SUCCESS ) {
        slurm_error("auto_tmpdir: no step id associated with job %u??", job_id);
        return rc;
    }
    spank_get_item(spank_ctxt, S_TASK_ID, &task_id);

    slurm_verbose("slurm_spank_task_init(%u, %u, %u)", job_id, job_step_id, task_id);

    /* What user should we function as? */
    if ((rc = spank_get_item (spank_ctxt, S_JOB_UID, &jobUid)) != ESPANK_SUCCESS) {
        slurm_error ("auto_tmpdir: unable to get job's user id");
        return rc;
    }
    if ((rc = spank_get_item (spank_ctxt, S_JOB_GID, &jobGid)) != ESPANK_SUCCESS) {
        slurm_error ("auto_tmpdir: unable to get job's group id");
        return rc;
    }

    /* Drop privileges: */
    if ( jobGid != savedGid ) {
        if ( setegid(jobGid) != 0 ) {
            if ( didSetUid ) seteuid(savedUid);
            slurm_error("auto_tmpdir: unable to %d -> setegid(%d) (errno = %d)", savedGid, jobGid, errno);
            return ESPANK_ERROR;
        }
        didSetGid = 1;
        slurm_verbose("auto_tmpdir:  changed to gid %d", jobGid);
    }
    if ( jobUid != savedUid ) {
        if ( seteuid(jobUid) != 0 ) {
            slurm_error("auto_tmpdir: unable to %d -> seteuid(%d) (errno = %d)", savedUid, jobUid, errno);
            return ESPANK_ERROR;
        }
        didSetUid = 1;
        slurm_verbose("auto_tmpdir:  changed to uid %d", jobUid);
    }

    tmpdirlen = _mktmpdir(tmpdir, sizeof(tmpdir), job_id, job_step_id, task_id);

    /* Restore privileges: */
    if ( didSetUid ) seteuid(savedUid);
    if ( didSetGid ) setegid(savedGid);

    if ( tmpdirlen > 0 ) {
        if ( (rc = spank_setenv(spank_ctxt, "TMPDIR", tmpdir, tmpdirlen)) != ESPANK_SUCCESS ) {
            slurm_error("setenv(TMPDIR, \"%s\"): %m", tmpdir);
            return rc;
        }
        slurm_verbose("auto_tmpdir: TMPDIR = %s", tmpdir);
    }
    return ESPANK_SUCCESS;
}


/*
 * @function __cleanup_tmpdir
 *
 * Remove the TMPDIR associated with the tuple (job_id, job_step_id, local_task_id).
 */
int
__cleanup_tmpdir(
    spank_t       spank_ctxt,
    uint32_t      job_id,
    uint32_t      job_step_id,
    uint32_t      local_task_id
)
{
    int           rc = ESPANK_SUCCESS;
    char          tmpdir[PATH_MAX];
    int           tmpdirlen = 0;
    uid_t         jobUid = -1, savedUid = geteuid();
    gid_t         jobGid = -1, savedGid = getegid();
    int           didSetUid = 0, didSetGid = 0;

    /* What user should we function as? */
    if ((rc = spank_get_item (spank_ctxt, S_JOB_UID, &jobUid)) != ESPANK_SUCCESS) {
        slurm_error ("auto_tmpdir: __cleanup_tmpdir: unable to get job's user id");
        return rc;
    }
    if ((rc = spank_get_item (spank_ctxt, S_JOB_GID, &jobGid)) != ESPANK_SUCCESS) {
        slurm_error ("auto_tmpdir: __cleanup_tmpdir: unable to get job's group id");
        return rc;
    }

    /* Drop privileges: */
    if ( jobGid != savedGid ) {
        if ( setegid(jobGid) != 0 ) {
            slurm_error("auto_tmpdir: __cleanup_tmpdir: unable to %d -> setegid(%d) (errno = %d)", savedGid, jobGid, errno);
            if ( didSetUid ) seteuid(savedUid);
            return ESPANK_ERROR;
        }
        didSetGid = 1;
        slurm_verbose("auto_tmpdir: __cleanup_tmpdir: changed to gid %d", jobGid);
    }
    if ( jobUid != savedUid ) {
        if ( seteuid(jobUid) != 0 ) {
            slurm_error("auto_tmpdir: __cleanup_tmpdir: unable to %d -> seteuid(%d) (errno = %d)", savedUid, jobUid, errno);
            return ESPANK_ERROR;
        }
        didSetUid = 1;
        slurm_verbose("auto_tmpdir: __cleanup_tmpdir: changed to uid %d", jobUid);
    }

    /* Create the path for this job sub-step: */
    tmpdirlen = _sprint_tmpdir(tmpdir, sizeof(tmpdir), job_id, job_step_id, local_task_id, 1, NULL);
    if ( tmpdirlen > 0 ) {
        /* If we're ignoring sub-step directories, then this can ONLY be deleted when
         * the extern/batch step exits:
         */
        if ( should_create_per_step_tmpdirs || ((job_step_id == SLURM_BATCH_SCRIPT) || (job_step_id == SLURM_EXTERN_CONT)) ) {
            struct stat   finfo;

            if ( (stat(tmpdir, &finfo) == 0) && S_ISDIR(finfo.st_mode) ) {
                if ( _rmdir_recurse(tmpdir) != 0 ) {
                    slurm_info("auto_tmpdir: __cleanup_tmpdir: Unable to remove TMPDIR at exit (failure in _rmdir_recurse(%s))", tmpdir);
                    rc = ESPANK_ERROR;
                } else {
                    slurm_verbose("auto_tmpdir: __cleanup_tmpdir: rm -rf %s", tmpdir);
                }
            } else {
                slurm_verbose("auto_tmpdir: __cleanup_tmpdir: failed stat check of %s (st_mode = %x, errno = %d)", tmpdir, finfo.st_mode, errno);
            }
        }
    }

    /* Restore privileges: */
    if ( didSetUid ) seteuid(savedUid);
    if ( didSetGid ) setegid(savedGid);

    return rc;
}


/*
 * @function slurm_spank_task_exit
 *
 * Remove each job step's TMPDIR as it exits.  TMPDIR is not touched if
 * this is the batch or extern step.
 *
 * Note that if shared storage is being compiled in and the job does NOT
 * use per-node sub-directories, we cannot remove the job step directory
 * until all steps on all nodes have exited -- but we can't synchronize
 * against anything, so we just forego removing the step directories until
 * the job itself exits.
 *
 * (Called as root as each task's process is reaped with wait().)
 */
int
slurm_spank_task_exit(
    spank_t       spank_ctxt,
    int           ac,
    char          **av
)
{
    int           rc = ESPANK_SUCCESS;

#ifdef WITH_SHARED_STORAGE
    if ( should_remove_tmpdir && should_add_pernode_on_shared ) {
#else
    if ( should_remove_tmpdir ) {
#endif
        uint32_t      job_id, job_step_id, task_id;

        /* Get the job id and step id: */
        if ( (rc = spank_get_item(spank_ctxt, S_JOB_ID, &job_id)) != ESPANK_SUCCESS ) {
            slurm_error("auto_tmpdir: slurm_spank_task_exit: no job id associated with job??");
            return rc;
        }
        if ( (rc = spank_get_item(spank_ctxt, S_JOB_STEPID, &job_step_id)) != ESPANK_SUCCESS ) {
            slurm_error("auto_tmpdir: slurm_spank_task_exit: no step id associated with job %u??", job_id);
            return rc;
        }
        spank_get_item(spank_ctxt, S_TASK_ID, &task_id);

        slurm_verbose("slurm_spank_task_exit(%u, %u, %u)", job_id, job_step_id, task_id);
        if ( (job_step_id != SLURM_BATCH_SCRIPT) && (job_step_id != SLURM_EXTERN_CONT) ) rc = __cleanup_tmpdir(spank_ctxt, job_id, job_step_id, task_id);
    }
    return rc;
}


/*
 * @function slurm_spank_exit
 *
 * Remove the job TMPDIR as it exits.
 *
 * (Called as root user.)
 */
int
slurm_spank_exit(
    spank_t       spank_ctxt,
    int           ac,
    char          **av
)
{
    int           rc = ESPANK_SUCCESS;

    if ( should_remove_tmpdir ) {
        if ( spank_remote(spank_ctxt) ) {
            uint32_t    job_id, job_step_id;

            /* Get the job id and step id: */
            if ( (rc = spank_get_item(spank_ctxt, S_JOB_ID, &job_id)) != ESPANK_SUCCESS ) {
                slurm_error("auto_tmpdir: slurm_spank_exit: no job id associated with job??");
                return rc;
            }
            if ( (rc = spank_get_item(spank_ctxt, S_JOB_STEPID, &job_step_id)) != ESPANK_SUCCESS ) {
                slurm_error("auto_tmpdir: slurm_spank_task_exit: no step id associated with job %u??", job_id);
                return rc;
            }

            slurm_verbose("slurm_spank_exit(%u, %u)", job_id, job_step_id);
            if ( (job_step_id == SLURM_BATCH_SCRIPT) || (job_step_id == SLURM_EXTERN_CONT) ) __cleanup_tmpdir(spank_ctxt, job_id, job_step_id, SLURM_BATCH_SCRIPT);
        }
    }
    return rc;
}
