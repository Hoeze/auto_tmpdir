/*
 * auto_tmpdir
 *
 * SLURM SPANK plugin that automates the process of creating/destroying
 * temporary directories for jobs/steps.
 */

#include "fs-utils.h"

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(auto_tmpdir, 1)

/*
 * Options bit vector:
 */
static auto_tmpdir_fs_options_t     auto_tmpdir_options = 0;

/*
 * Filesystem bind mount info:
 */
static auto_tmpdir_fs_ref           auto_tmpdir_fs_info = NULL;

/*
 * Which job step should cleanup?
 */
static uint32_t                     auto_tmpdir_cleanup_in_step = SLURM_EXTERN_CONT;

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
    auto_tmpdir_options |= auto_tmpdir_fs_options_should_not_delete;
    slurm_verbose("auto_tmpdir:  will not remove tempororary directories");
    return ESPANK_SUCCESS;
}

#ifdef AUTO_TMPDIR_ENABLE_SHARED_TMPDIR
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
            auto_tmpdir_options |= auto_tmpdir_fs_options_should_use_per_host;
        } else {
            slurm_error("auto_tmpdir:  invalid --use-shared-tmpdir optional value: %s", optarg);
            return ESPANK_BAD_ARG;
        }
    }

    auto_tmpdir_options |= auto_tmpdir_fs_options_should_use_shared;
    slurm_verbose("auto_tmpdir:  will use shared tempororary directory under `%s`", AUTO_TMPDIR_DEFAULT_SHARED_PREFIX);

    return ESPANK_SUCCESS;
}
#endif

/*
 * Options available to this spank plugin:
 */
struct spank_option spank_options[] =
    {
        { "no-rm-tmpdir", NULL,
            "Do not automatically remove temporary directories for the job/steps.",
            0, 0, (spank_opt_cb_f) _opt_no_rm_tmpdir },

#ifdef AUTO_TMPDIR_ENABLE_SHARED_TMPDIR
        { "use-shared-tmpdir", NULL,
#ifdef HAVE_SPANK_JOB_ARRAY_IDS
            "Create temporary directories on shared storage (overridden by --tmpdir).  Use \"--use-shared-tmpdir=per-node\" to create unique sub-directories for each node allocated to the job (e.g. <base><job-id>{.<array-task-id>}/<nodename>).",
#else
            "Create temporary directories on shared storage.  Use \"--use-shared-tmpdir=per-node\" to create unique sub-directories for each node allocated to the job (e.g. <base><job-id>/<nodename>).",
#endif
            2, 0, (spank_opt_cb_f) _opt_use_shared_tmpdir },
#endif

        SPANK_OPTIONS_TABLE_END
    };


/**/


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
    spank_t         spank_ctxt,
    int             argc,
    char            *argv[]
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
                rc = _opt_no_rm_tmpdir(0, v, 1);
            }
#ifdef AUTO_TMPDIR_ENABLE_SHARED_TMPDIR
            if ( (rc == ESPANK_SUCCESS) && (spank_getenv(spank_ctxt, "SLURM_SPANK__SLURM_SPANK_OPTION_auto_tmpdir_use_shared_tmpdir", v, sizeof(v)) == ESPANK_SUCCESS) ) {
                rc = _opt_use_shared_tmpdir(0, v, 1);
            }
#endif
            break;
        }

    }
    return rc;
}


/*
 * @function slurm_spank_init_post_opt
 *
 */
int
slurm_spank_init_post_opt(
    spank_t         spank_ctxt,
    int             argc,
    char            *argv[]
)
{
    int             rc = ESPANK_SUCCESS;

    /* We only want to run in the remote context: */
    if ( spank_remote(spank_ctxt) ) {
        auto_tmpdir_fs_info = auto_tmpdir_fs_init(spank_ctxt, argc, argv, auto_tmpdir_options);

        rc = ESPANK_ERROR;
        if ( auto_tmpdir_fs_info && (auto_tmpdir_fs_bind_mount(auto_tmpdir_fs_info) == 0) ) {
            const char      *tmpdir = auto_tmpdir_fs_get_tmpdir(auto_tmpdir_fs_info);

            if ( ! tmpdir || ((rc = spank_setenv(spank_ctxt, "TMPDIR", tmpdir, strlen(tmpdir))) != ESPANK_SUCCESS) ) {
                slurm_error("auto_tmpdir::slurm_spank_init_post_opt: setenv(TMPDIR, \"/tmp\") failed (%m)");
            }
        }
    }
    return rc;
}


/*
 * @function slurm_spank_exit
 *
 * Remove the job's temporary directory and any bind mountpoints.
 */
int
slurm_spank_exit(
    spank_t         spank_ctxt,
    int             argc,
    char            *argv[]
)
{
    if ( spank_remote(spank_ctxt) && auto_tmpdir_fs_info ) {
        uint32_t    job_step_id;
        int         rc;

        if ( (rc = spank_get_item(spank_ctxt, S_JOB_STEPID, &job_step_id)) != ESPANK_SUCCESS ) {
            slurm_error("auto_tmpdir::slurm_spank_exit: no step id associated with job??");
            return rc;
        }
        slurm_debug("auto_tmpdir::slurm_spank_exit: checking for cleanup, step %u == %u", job_step_id, auto_tmpdir_cleanup_in_step);
        if ( (job_step_id == auto_tmpdir_cleanup_in_step) && (auto_tmpdir_fs_fini(auto_tmpdir_fs_info, 0) != 0) ) return ESPANK_ERROR;
    }
    return ESPANK_SUCCESS;
}
