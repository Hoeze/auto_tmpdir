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

/* 
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(auto_tmpdir, 1)

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
      
    SPANK_OPTIONS_TABLE_END
  };


/**/


/*
 * @function _get_base_tmpdir
 *
 * Returns the configured base directory for temporary directories
 * or /tmp by default.
 *
 */
const char*
_get_base_tmpdir()
{
  const char      *tmpdir;
  
  if ( base_tmpdir ) return base_tmpdir;
  return "/tmp";
}


/*
 * @function _mktmpdir
 *
 * Given a job id and step id, create the temporary directory.
 *
 */
int
_mktmpdir(
  spank_t         spank,
  char            *outTmpDir,
  size_t          outTmpDirLen
)
{
  uint32_t      job_id = 0;
  uint32_t      job_step_id = 0;
  const char    *tmpdir = NULL;
  int           actual_len = 0;
  
  /* Get the job id and step id: */
  if ( spank_get_item(spank, S_JOB_ID, &job_id) != ESPANK_SUCCESS ) {
    slurm_error("auto_tmpdir: no job id associated with job??");
    return (-1);
  }
  if ( spank_get_item(spank, S_JOB_STEPID, &job_step_id) != ESPANK_SUCCESS ) {
    slurm_error("auto_tmpdir: no step id associated with job %u??", job_id);
    return (-1);
  }
  
  /* Retrieve the base temp directory: */
  tmpdir = _get_base_tmpdir();
  
  /* Decide which format the directory should use and determine string length: */
  if ( ! should_create_per_step_tmpdirs || (job_step_id == SLURM_BATCH_SCRIPT) ) {
    actual_len = snprintf(outTmpDir, outTmpDirLen, "%s/%u", tmpdir, job_id);
  } else {
    actual_len = snprintf(outTmpDir, outTmpDirLen, "%s/%u/%u", tmpdir, job_id, job_step_id);
  }
  
  /* If the snprintf() failed then we've got big problems: */
  if ( (actual_len < 0) || (actual_len >= outTmpDirLen) ) {
    slurm_error("auto_tmpdir: Failure while creating new tmpdir path: %d", actual_len);
    return (-1);
  } else {
    struct stat   finfo;
    
    /* Build the path, making sure each component exists: */
    strncpy(outTmpDir, tmpdir, outTmpDirLen);
    if ( (stat(outTmpDir, &finfo) == 0) && S_ISDIR(finfo.st_mode) ) {
      /* At the least we'll need the job directory: */
      actual_len = snprintf(outTmpDir, outTmpDirLen, "%s/%u", tmpdir, job_id);
      if ( stat(outTmpDir, &finfo) != 0 ) {
        if ( mkdir(outTmpDir, 0700) != 0 ) {
          slurm_error("auto_tmpdir: failed creating job tmpdir: %s", outTmpDir);
          return (-1);
        }
        stat(outTmpDir, &finfo);
      }
      if ( ! S_ISDIR(finfo.st_mode) ) {
        slurm_error("auto_tmpdir: job tmpdir is not a directory: %s", outTmpDir);
        return (-1);
      }
      
      /* If this isn't the batch portion of a job, worry about the step subdir: */
      if ( should_create_per_step_tmpdirs && (job_step_id != SLURM_BATCH_SCRIPT) ) {
        actual_len = snprintf(outTmpDir, outTmpDirLen, "%s/%u/%u", tmpdir, job_id, job_step_id);
        if ( stat(outTmpDir, &finfo) != 0 ) {
          if ( mkdir(outTmpDir, 0700) != 0 ) {
            slurm_error("auto_tmpdir: failed creating step tmpdir: %s", outTmpDir);
            return (-1);
          }
          stat(outTmpDir, &finfo);
        }
        if ( ! S_ISDIR(finfo.st_mode) ) {
          slurm_error("auto_tmpdir: step tmpdir is not a directory: %s", outTmpDir);
          return (-1);
        }
      }
    } else {
      slurm_error("auto_tmpdir: base tmpdir is not a directory: %s", tmpdir);
      return (-1);
    }
  }
  
  return actual_len;
}


int
_rmdir_recurse(
  const char      *path,
  uid_t           match_uid
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
    slurm_error("auto_tmpdir: _rmdir_recurse(): Failed to open file traversal context on %s: %s", path, strerror(errno));
    return (-1);
  }
  
  //
  // Read the room item -- should be a directory owned by match_uid:
  //
  if ( (ftsItem = fts_read(ftsPtr)) ) {
    switch ( ftsItem->fts_info ) {
      case FTS_NS:
      case FTS_DNR:
      case FTS_ERR: {
        slurm_verbose("auto_tmpdir: _rmdir_recurse(%s): directory does not exist", path);
        break;
      }
      case FTS_D: {
        //
        // We're entering a directory -- exactly what we want!
        //
        if ( ftsItem->fts_statp->st_uid == match_uid ) {
          while ( (ftsItem = fts_read(ftsPtr)) ) {
            switch ( ftsItem->fts_info ) {
              case FTS_NS:
              case FTS_DNR:
              case FTS_ERR:
                slurm_error("auto_tmpdir: _rmdir_recurse(): Error in fts_read(%s): %s\n", ftsItem->fts_accpath, strerror(ftsItem->fts_errno));
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
                  slurm_error("auto_tmpdir: _rmdir_recurse(): Failed to remove %s: %s\n", ftsItem->fts_path, strerror(errno));
                  rc = -1;
                }
                break;
            }
          }
        } else {
          slurm_error("auto_tmpdir: _rmdir_recurse(): Failed to remove %s: not owned by job user (%d != %d)\n", path, ftsItem->fts_statp->st_uid, match_uid);
          rc = -1;
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
  
  if ( spank_context() == S_CTX_ALLOCATOR ) {
    struct spank_option   *o = spank_options;
    
    while ( o->name && (rc == ESPANK_SUCCESS) ) rc = spank_option_register(spank_ctxt, o++);
  }
  
  return rc;
}


/*
 * @function slurm_spank_local_user_init
 *
 * Set job-specific TMPDIR in environment.  For batch scripts the path
 * uses just the job id; for all others, the path uses the job id, a dot,
 * and the job step id.  The value of TMPDIR handed to us by SLURM is
 * the base path for the new TMPDIR; if SLURM doesn't hand us a TMPDIR
 * then we default to using /tmp as our base directory.
 *
 * This function does not actually create the directory.
 *
 * (Called from srun after allocation before launch.)
 */
int
slurm_spank_local_user_init(
  spank_t       spank_ctxt,
  int           argc,
  char          *argv[]
)
{
  char          tmpdir[PATH_MAX];
  int           tmpdirlen = _mktmpdir(spank_ctxt, tmpdir, sizeof(tmpdir));
  
  if ( tmpdirlen > 0 ) {
    if ( setenv("TMPDIR", tmpdir, 1) < 0 ) {
      slurm_error("setenv(TMPDIR, \"%s\"): %m", tmpdir);
      return (-1);
    }
    slurm_verbose("auto_tmpdir: TMPDIR = %s", tmpdir);
  }
  return (0);
}


/*
 * @function slurm_spank_user_init
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
slurm_spank_user_init(
  spank_t       spank_ctxt,
  int           argc,
  char          *argv[]
)
{
  char          tmpdir[PATH_MAX];
  int           tmpdirlen = _mktmpdir(spank_ctxt, tmpdir, sizeof(tmpdir));
  
  if ( tmpdirlen > 0 ) {
    if ( spank_setenv(spank_ctxt, "TMPDIR", tmpdir, tmpdirlen) != ESPANK_SUCCESS ) {
      slurm_error("setenv(TMPDIR, \"%s\"): %m", tmpdir);
      return (-1);
    }
    slurm_verbose("auto_tmpdir: TMPDIR = %s", tmpdir);
  }
  return (0);
}


/*
 * @function slurm_spank_exit
 *
 * Remove the job's TMPDIR to keep temporary volumes neat and tidy.
 *
 * (Called as root user after tasks have exited.)
 */
int
slurm_spank_exit(
  spank_t       spank_ctxt,
  int           ac,
  char          **av
)
{
  if ( should_remove_tmpdir ) {
    if ( spank_remote(spank_ctxt) ) {
      char            tmpdir[PATH_MAX];
      
      if ( spank_getenv(spank_ctxt, "TMPDIR", tmpdir, sizeof(tmpdir)) == ESPANK_SUCCESS ) {
        uid_t         jobUid = -1;
        struct stat   finfo;

        if (spank_get_item (spank_ctxt, S_JOB_UID, &jobUid) != ESPANK_SUCCESS) {
          slurm_error ("auto_tmpdir: remote: unable to get job's user id");
          return (-1);
        }
        
        if ( (stat(tmpdir, &finfo) == 0) && S_ISDIR(finfo.st_mode) ) {
          if ( _rmdir_recurse(tmpdir, jobUid) != 0 ) {
            slurm_error("auto_tmpdir: remote: Unable to remove TMPDIR at exit (failure in _rmdir_recurse(%s,%d))", tmpdir, jobUid);
            return (-1);
          }
          slurm_verbose("auto_tmpdir: remote: rm -rf %s", tmpdir);
        } else {
          slurm_error("auto_tmpdir: remote: failed stat check of %s (uid = %d, st_mode = %x, errno = %d)", tmpdir, jobUid, finfo.st_mode, errno);
        }
      }
    } else if ( should_create_per_step_tmpdirs ) {
      const char      *tmpdir = getenv("TMPDIR");
      
      if ( tmpdir ) {
        uid_t         jobUid = geteuid();
        struct stat   finfo;
        
        /* We don't care if the directory doesn't exist anymore... */
        if ( stat(tmpdir, &finfo) == 0 ) {
          /*  ...but if it does, let's get rid of it. */
          if ( S_ISDIR(finfo.st_mode) ) {
            if ( _rmdir_recurse(tmpdir, jobUid) != 0 ) {
              slurm_error("auto_tmpdir: local: Unable to remove TMPDIR at exit (failure in _rmdir_recurse(%s,%d))", tmpdir, jobUid);
              return (-1);
            }
            slurm_verbose("auto_tmpdir: local: rm -rf %s", tmpdir);
          }
        }
      }
    }
  }
  return (0);
}
