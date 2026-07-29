#define MODULE_LOG_PREFIX "sched"  // Logging prefix for all scheduler messages

#include "globals.h"
#include "ncam-sched.h"
#include "ncam-string.h"
#include "ncam-time.h"
#include "ncam-lock.h"
#include "ncam-sched-priv.h"

/*
 * Scheduler Main Loop, Job Worker Threads, Lifecycle
 *
 * This file now only owns the runtime side of the scheduler: the global
 * context, the job worker thread body, the polling loop, and
 * init/shutdown. Config parsing lives in ncam-sched-cfg.c and the NCam
 * API HTTP client lives in ncam-sched-http.c (see ncam-sched-priv.h for
 * the shared types/declarations that tie the three files together).
 */

extern char cs_confdir[128];

/*
 * Global Scheduler Instance
 * Singleton instance containing all scheduler state. Declared extern in
 * ncam-sched-priv.h so ncam-sched-cfg.c can populate jobs[] while parsing
 * task.cfg.
 */
sched_ctx_t g_sched = {0};

/*
 * Job Thread Entry Point
 * Purpose: Executes job steps sequentially in dedicated thread
 * Process: Iterates through steps, handles each type, updates job state on completion
 * Thread Safety: Uses mutex for state transitions, detaches on completion
 */
static void *sched_job_run(void *arg)
{
    job_t *job = (job_t *)arg;                           // Cast thread argument
    if (!job) return NULL;                               // Safety check
    
    // Step 1: Transition to RUNNING state (thread-safe)
    cs_writelock(__func__, &g_sched.lock);
    job->state = JOB_RUNNING;
    cs_writeunlock(__func__, &g_sched.lock);
    
    // Step 2: Execute each job step in sequence
    int i;
    for (i = 0; i < job->step_count; i++) {
        switch (job->steps[i].type) {
            case STEP_API:
                // Execute API call, log failure but continue execution
                if (ncam_api_call(job->steps[i].data.api_query) != API_OK) {
                    cs_log("[%s] API call failed at step %d", job->name, i + 1);
                }
                break;
                
            case STEP_LOG:
                // Write formatted message to system log
                cs_log("[%s] %s", job->name, job->steps[i].data.log_msg);
                break;
                
            case STEP_SLEEP:
                // Pause execution for specified duration
                if (job->steps[i].data.sleep_sec > 0) {
                    sleep(job->steps[i].data.sleep_sec);
                }
                break;
        }
    }
    
    // Step 3: Post-execution processing
    time_t now = cs_time();                              // Capture completion time
    
    // Step 4: Update job state (thread-safe)
    cs_writelock(__func__, &g_sched.lock);
    job->is_running = 0;                                 // Clear running flag
    job->last_run = now;                                 // Record completion time
    job->run_count++;                                    // Increment execution counter
    
    // Step 5: Determine next state based on schedule type
    if (job->has_datetime) {                             // One-time job
        job->state = JOB_DONE;                           // Terminal state
        job->enabled = 0;                                // Disable future execution
    }
    else if (job->loop) {                                // Continuous loop mode
        job->state = JOB_IDLE;
        // Use interval or minimum 1 second
        job->next_run = now + (job->interval_sec > 0 ? job->interval_sec : 1);
    }
    else if (job->interval_sec > 0) {                    // Fixed interval
        job->state = JOB_IDLE;
        job->next_run = now + job->interval_sec;
    }
    else if (job->has_daily) {                           // Daily schedule
        job->state = JOB_IDLE;
        job->next_run = sched_calc_daily(job->hour, job->minute);
    }
    else if (job->has_weekly) {                          // Weekly schedule
        job->state = JOB_IDLE;
        job->next_run = sched_calc_weekly(job->weekday, job->hour, job->minute);
    }
    else {                                               // Invalid configuration
        job->state = JOB_DONE;                           // Terminal state
        job->enabled = 0;                                // Disable
    }
    
    cs_writeunlock(__func__, &g_sched.lock);
    return NULL;                                         // Thread exit
}

/*
 * Main Scheduler Loop
 * Purpose: Monitors all jobs, triggers execution when scheduled
 * Process: Polls job states every 200ms, spawns threads for ready jobs
 * Thread: Runs continuously until shutdown signal received
 */
static void *sched_loop(void *UNUSED(arg))
{
    while (g_sched.running) {                            // Continue until shutdown
        time_t now = cs_time();                          // Current timestamp
        
        // Thread-safe job processing
        cs_writelock(__func__, &g_sched.lock);
        int i;
        for (i = 0; i < g_sched.job_count; i++) {
            job_t *job = &g_sched.jobs[i];
            
            // Skip conditions
            if (!job->enabled || job->is_running || job->step_count == 0)
                continue;
                
            // Check if job should transition to READY state
            if (job->state == JOB_IDLE && job->next_run <= now) {
                job->state = JOB_READY;
            }
            
            // Launch job thread if ready and not already running
            if (job->state == JOB_READY && !job->is_running) {
                job->is_running = 1;                     // Set running flag
                if (pthread_create(&job->thread, NULL, sched_job_run, job) != 0) {
                    job->is_running = 0;                 // Reset on failure
                    cs_log("[%s] Failed to create thread", job->name);
                } else {
                    pthread_detach(job->thread);         // Auto-cleanup on completion
                }
            }
        }
        cs_writeunlock(__func__, &g_sched.lock);
        
        usleep(200000);                                  // Poll every 200ms
    }
    return NULL;                                         // Shutdown complete
}

/*
 * Scheduler Initialization
 * Purpose: Bootstrap scheduler subsystem
 * Process: Checks configuration, loads task.cfg, creates threads and locks
 * Returns: 0 on success, -1 on critical failure
 */
int32_t ncam_sched_init(void)
{
    // Feature check
    if (!cfg.task_enabled)
        return 0;                                        // Scheduler disabled

    // Construct configuration file path
    char task_path[256];
    snprintf(task_path, sizeof(task_path), "%stask.cfg", cs_confdir);

    // Open configuration file
    FILE *fp = fopen(task_path, "r");
    if (!fp) {
        cs_log("Task file not found: %s", task_path);
        return 0;                                        // Non-critical (optional feature)
    }

    // Initialize global scheduler context
    memset(&g_sched, 0, sizeof(sched_ctx_t));
    cs_lock_create(__func__, &g_sched.lock, "sched_lock", 5000);

    // Load configuration
    int ret = sched_load_cfg(fp);
    fclose(fp);                                          // Always close file

    // Validate loaded configuration
    if (ret != 0 || g_sched.job_count == 0) {
        cs_lock_destroy(__func__, &g_sched.lock);        // Cleanup
        NULLFREE(g_sched.jobs);                          // Free jobs array if allocated
        return 0;                                        // No jobs to schedule
    }

    // Start main scheduler thread
    g_sched.running = 1;                                 // Set run flag
    if (pthread_create(&g_sched.thread, NULL, sched_loop, NULL) != 0) {
        g_sched.running = 0;                             // Reset on failure
        cs_lock_destroy(__func__, &g_sched.lock);        // Cleanup
        NULLFREE(g_sched.jobs);                          // Free jobs array if allocated
        return -1;                                       // Critical failure
    }

    cs_log("Scheduler started with %d job(s)", g_sched.job_count);
    return 0;                                            // Success
}

/*
 * Scheduler Graceful Shutdown
 * Purpose: Cleanly terminate scheduler and all worker threads
 * Process: Signals stop, joins main thread, waits for workers, cleans resources
 * Safety: Ensures no resource leaks, waits for running jobs to complete
 */
void ncam_sched_shutdown(void)
{
    if (!g_sched.running) return;                       // Already stopped

    // Step 1: Signal main scheduler thread to stop
    g_sched.running = 0;
    pthread_join(g_sched.thread, NULL);                  // Wait for main thread

    // Step 2: Wait for worker threads to complete (max 10 seconds)
    int w, i;
    for (w = 0; w < 100; w++) {
        int running = 0;
        cs_writelock(__func__, &g_sched.lock);
        for (i = 0; i < g_sched.job_count; i++)
            if (g_sched.jobs[i].is_running) running++;   // Count active workers
        cs_writeunlock(__func__, &g_sched.lock);

        if (!running) break;                             // All threads completed
        usleep(100000);                                  // Wait 100ms
    }

    // Step 3: Free allocated resources
    for (i = 0; i < g_sched.job_count; i++)
        if (g_sched.jobs[i].steps)
            NULLFREE(g_sched.jobs[i].steps);             // Free each job's step array

    NULLFREE(g_sched.jobs);                              // Free the jobs array itself

    // Step 4: Destroy synchronization primitives
    cs_lock_destroy(__func__, &g_sched.lock);
    cs_log("Scheduler stopped");                         // Final status
}
