#ifndef NCAM_SCHED_PRIV_H
#define NCAM_SCHED_PRIV_H

/*
 * Internal scheduler types, limits and cross-file declarations.
 *
 * Shared between:
 *   ncam-sched.c      - main loop, job worker threads, init/shutdown
 *   ncam-sched-cfg.c  - task.cfg parsing / job-step storage
 *   ncam-sched-http.c - NCam API HTTP client + Digest authentication
 *
 * This header is intentionally NOT the public API of the scheduler
 * feature - other modules should only ever include ncam-sched.h, which
 * exposes just ncam_sched_init()/ncam_sched_shutdown().
 */

#define MAX_JOBS        128     // Maximum number of concurrent jobs
#define MAX_STEPS       256     // Maximum steps per job (prevents infinite loops)
#define MAX_NAME        128     // Maximum job name length
#define MAX_QUERY       8192    // Maximum HTTP API query length
#define MAX_MSG         512     // Maximum log message length

typedef enum {
    API_OK = 0,     // Operation completed successfully
    API_ERR = -1    // Operation failed (network, auth, or server error)
} api_result_t;

/*
 * Job State Machine Enumeration
 * Tracks job lifecycle from scheduling through completion
 */
typedef enum {
    JOB_IDLE,      // Initial state: waiting for next scheduled trigger time
    JOB_READY,     // Trigger condition met: ready for thread execution
    JOB_RUNNING,   // Currently executing in worker thread
    JOB_DONE       // Execution completed (terminal state for non-recurring jobs)
} job_state_t;

/*
 * Step Type Enumeration
 * Defines available action types within job workflows
 */
typedef enum {
    STEP_API,      // Execute NCam API HTTP call
    STEP_LOG,      // Write entry to system log
    STEP_SLEEP     // Pause execution for specified duration
} step_type_t;

/*
 * Job Step Structure
 * Represents individual action within job execution sequence
 */
typedef struct {
    step_type_t type;      // Type of step (determines which union member is active)
    union {                // Step-specific data payload
        char api_query[MAX_QUERY];  // HTTP API endpoint and parameters
        char log_msg[MAX_MSG];      // Formatted log message text
        int sleep_sec;              // Sleep duration in seconds
    } data;
} job_step_t;

/*
 * Job Definition Structure
 * Complete configuration and runtime state for a scheduled task
 */
typedef struct {
    char name[MAX_NAME];           // Unique job identifier for logging
    int enabled;                   // Activation flag (0=disabled, 1=enabled)
    int loop;                      // Continuous execution mode (1=enabled)
    int interval_sec;              // Fixed interval between executions (seconds)

    // Daily scheduling parameters
    int hour, minute;              // Time of day for execution (24h format)

    // Weekly scheduling parameters
    int weekday;                   // Day of week (0=Sunday, 6=Saturday)

    // Schedule type flags (mutually exclusive)
    int has_daily;                 // Daily schedule enabled
    int has_weekly;                // Weekly schedule enabled
    int has_datetime;              // One-time absolute schedule enabled

    time_t datetime_target;        // Absolute execution timestamp for one-time jobs

    // Step configuration
    job_step_t *steps;             // Dynamic array of job steps (grown on demand)
    int step_count;                // Number of configured steps
    int step_capacity;             // Currently allocated capacity of steps[]

    // Runtime statistics
    int run_count;                 // Total executions completed
    time_t next_run;               // Next scheduled execution time
    time_t last_run;               // Last execution completion time

    // State management
    job_state_t state;             // Current state in job lifecycle
    pthread_t thread;              // Worker thread handle
    volatile int is_running;       // Thread-safe execution flag
} job_t;

/*
 * Scheduler Context Structure
 * Global container for all scheduler state and control variables.
 * jobs[] is dynamically grown (see sched_ensure_job_capacity() in
 * ncam-sched-cfg.c) instead of a fixed MAX_JOBS-sized array, and is only
 * ever resized during the single-threaded task.cfg parse in
 * ncam_sched_init() - before the scheduler thread or any job worker
 * thread exists - so no other thread ever holds a pointer into it while
 * it can still move.
 */
typedef struct {
    job_t *jobs;                   // Dynamically-grown array of job definitions
    int job_count;                 // Active job count
    int job_capacity;              // Currently allocated capacity of jobs[]
    pthread_t thread;              // Main scheduler thread handle
    CS_MUTEX_LOCK lock;            // Mutex for thread-safe job access
    volatile int running;          // Global scheduler run flag
} sched_ctx_t;

/* Single global scheduler instance, defined in ncam-sched.c */
extern sched_ctx_t g_sched;

/* ncam-sched-cfg.c */
int sched_load_cfg(FILE *fp);
time_t sched_calc_daily(int hour, int minute);
time_t sched_calc_weekly(int weekday, int hour, int minute);

/* ncam-sched-http.c */
api_result_t ncam_api_call(const char *query);

#endif // NCAM_SCHED_PRIV_H
