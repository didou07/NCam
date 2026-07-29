#define MODULE_LOG_PREFIX "sched"  // Logging prefix for all scheduler messages

#include "globals.h"
#include "ncam-string.h"
#include "ncam-time.h"
#include "ncam-sched-priv.h"

/*
 * task.cfg Parsing / Job & Step Storage
 *
 * Split out of ncam-sched.c: everything here runs once, single-threaded,
 * during ncam_sched_init() before the scheduler thread or any job worker
 * thread exists. That's what makes it safe for sched_ensure_job_capacity()
 * and sched_ensure_step_capacity() to grow g_sched.jobs[] / job->steps[]
 * via realloc() - no other thread can be holding a pointer into either
 * array while it might still move.
 */

/*
 * Job Array Growth Helper
 * Purpose: Ensures g_sched.jobs has room for one more job, growing the
 * allocation on demand instead of a fixed MAX_JOBS-sized array.
 * Returns: 1 if room is available (or was made), 0 on failure/limit reached
 */
static int sched_ensure_job_capacity(void)
{
    if (g_sched.job_count >= MAX_JOBS)
        return 0;                                        // Hard limit reached

    if (g_sched.job_count < g_sched.job_capacity)
        return 1;                                        // Room already available

    int new_cap = g_sched.job_capacity ? g_sched.job_capacity * 2 : 8;
    if (new_cap > MAX_JOBS) new_cap = MAX_JOBS;

    job_t *new_jobs = realloc(g_sched.jobs, new_cap * sizeof(job_t));
    if (!new_jobs) return 0;                              // Allocation failed

    // Zero the newly added slots (realloc does not do this for us)
    memset(new_jobs + g_sched.job_capacity, 0,
           (new_cap - g_sched.job_capacity) * sizeof(job_t));

    g_sched.jobs = new_jobs;
    g_sched.job_capacity = new_cap;
    return 1;
}

/*
 * Step Array Growth Helper
 * Purpose: Ensures job->steps has room for one more step, growing the
 * allocation on demand instead of always pre-allocating MAX_STEPS entries.
 * Rationale: job_step_t is large (dominated by the 8KB api_query buffer in
 * its union), so unconditionally allocating MAX_STEPS (256) of them per job
 * wasted up to ~2MB per job regardless of how many steps it actually used -
 * a real concern on embedded targets that may define many small jobs.
 * Returns: 1 if room is available (or was made), 0 on failure/limit reached
 */
static int sched_ensure_step_capacity(job_t *job)
{
    if (job->step_count >= MAX_STEPS)
        return 0;                                        // Hard limit reached

    if (job->step_count < job->step_capacity)
        return 1;                                        // Room already available

    int new_cap = job->step_capacity ? job->step_capacity * 2 : 8;
    if (new_cap > MAX_STEPS) new_cap = MAX_STEPS;

    job_step_t *new_steps = realloc(job->steps, new_cap * sizeof(job_step_t));
    if (!new_steps) return 0;                            // Allocation failed

    job->steps = new_steps;
    job->step_capacity = new_cap;
    return 1;
}

/*
 * Daily Schedule Calculator
 * Purpose: Computes next execution timestamp for daily-recurring jobs
 * Process: Sets today's time to specified hour:minute, advances to tomorrow if passed
 * Returns: UNIX timestamp of next occurrence
 */
time_t sched_calc_daily(int hour, int minute)
{
    time_t now = cs_time();                              // Current system time
    struct tm tm_now;
    localtime_r(&now, &tm_now);                          // Convert to local time struct
    
    tm_now.tm_hour = hour;                               // Override hour
    tm_now.tm_min = minute;                              // Override minute
    tm_now.tm_sec = 0;                                   // Start of minute
    tm_now.tm_isdst = -1;                                // Let system determine DST
    
    time_t target = mktime(&tm_now);                     // Convert back to timestamp
    if (target <= now) target += 86400;                  // Advance to next day if passed
    return target;                                       // Next execution time
}

/*
 * Weekly Schedule Calculator
 * Purpose: Computes next execution timestamp for weekly-recurring jobs
 * Process: Calculates days until target weekday, adjusts time, handles wrap-around
 * Returns: UNIX timestamp of next occurrence
 */
time_t sched_calc_weekly(int weekday, int hour, int minute)
{
    time_t now = cs_time();                              // Current system time
    struct tm tm_now;
    localtime_r(&now, &tm_now);                          // Convert to local time
    
    int cur_wday = tm_now.tm_wday;                       // Current weekday (0=Sun)
    int days = (weekday - cur_wday + 7) % 7;             // Days until target weekday
    
    if (days == 0) {                                     // Today is target day
        tm_now.tm_hour = hour;                           // Set target hour
        tm_now.tm_min = minute;                          // Set target minute
        tm_now.tm_sec = 0;                               // Start of minute
        tm_now.tm_isdst = -1;                            // DST auto
        
        time_t target = mktime(&tm_now);                 // Today's target time
        if (target > now) return target;                 // If not passed, use today
        days = 7;                                        // Otherwise next week
    }
    
    // Calculate base for next week's occurrence
    time_t target = now + (days * 86400);                // Add days
    localtime_r(&target, &tm_now);                       // Convert for time setting
    
    tm_now.tm_hour = hour;                               // Set hour
    tm_now.tm_min = minute;                              // Set minute
    tm_now.tm_sec = 0;                                   // Start of minute
    tm_now.tm_isdst = -1;                                // DST auto
    
    return mktime(&tm_now);                              // Final timestamp
}

/*
 * Configuration File Parser
 * Purpose: Loads job definitions from task.cfg file
 * Process: Line-by-line parsing with section and key-value support
 * Validation: Enforces limits, validates formats, logs loaded configuration
 */
int sched_load_cfg(FILE *fp)
{
    if (!fp) return -1;                                  // Null file pointer

    // Initialize scheduler context
    g_sched.job_count = 0;
    char line[512];
    job_t *cur = NULL;                                   // Current job being parsed

    // Process each configuration line
    while (fgets(line, sizeof(line), fp)) {
        trim(line);                                      // Remove whitespace
        if (*line == '\0' || *line == '#') continue;     // Skip empty/comments

        // Detect job section header: [job:JobName]
        char name[MAX_NAME];
        if (sscanf(line, "[job:%127[^]]]", name) == 1) {
            if (!sched_ensure_job_capacity()) {
                cs_log("Job limit (%d) reached, '%s' and any further jobs ignored", MAX_JOBS, name);
                break;                                    // Capacity limit
            }
            
            // Initialize new job entry
            cur = &g_sched.jobs[g_sched.job_count++];
            memset(cur, 0, sizeof(job_t));               // Clear all fields
            cs_strncpy(cur->name, name, MAX_NAME);       // Set job name

            // Default values
            cur->enabled = 1;
            cur->state = JOB_IDLE;
            cur->next_run = cs_time();                   // Default to immediate
            // steps[] starts empty and grows on demand (see sched_ensure_step_capacity)
            continue;
        }

        if (!cur) continue;                              // Skip if no active job

        // Parse key=value pairs
        char *eq = strchr(line, '=');
        if (!eq) continue;                               // Invalid format
        
        *eq = '\0';                                      // Split key and value
        char *key = line;
        char *val = eq + 1;
        trim(key);                                       // Clean key
        trim(val);                                       // Clean value
        if (!*val) continue;                             // Skip empty values

        // Process known configuration keys
        if (strcmp(key, "enabled") == 0) {
            cur->enabled = atoi(val);                    // Convert to integer
        }
        else if (strcmp(key, "loop") == 0) {
            cur->loop = atoi(val);                       // Loop mode
        }
        else if (strcmp(key, "interval") == 0) {
            cur->interval_sec = atoi(val);               // Fixed interval
        }
        else if (strcmp(key, "time") == 0) {             // Daily schedule
            if (sscanf(val, "%d:%d", &cur->hour, &cur->minute) == 2) {
                cur->has_daily = 1;
                cur->next_run = sched_calc_daily(cur->hour, cur->minute);
            }
        }
        else if (strcmp(key, "weekly") == 0) {           // Weekly schedule
            char day[16];
            if (sscanf(val, "%15s %d:%d", day, &cur->hour, &cur->minute) == 3) {
                cur->has_weekly = 1;
                // Convert day name to numeric weekday
                if (strcasecmp(day, "sunday") == 0 || strcasecmp(day, "sun") == 0) cur->weekday = 0;
                else if (strcasecmp(day, "monday") == 0 || strcasecmp(day, "mon") == 0) cur->weekday = 1;
                else if (strcasecmp(day, "tuesday") == 0 || strcasecmp(day, "tue") == 0) cur->weekday = 2;
                else if (strcasecmp(day, "wednesday") == 0 || strcasecmp(day, "wed") == 0) cur->weekday = 3;
                else if (strcasecmp(day, "thursday") == 0 || strcasecmp(day, "thu") == 0) cur->weekday = 4;
                else if (strcasecmp(day, "friday") == 0 || strcasecmp(day, "fri") == 0) cur->weekday = 5;
                else if (strcasecmp(day, "saturday") == 0 || strcasecmp(day, "sat") == 0) cur->weekday = 6;
                else cur->has_weekly = 0;                // Invalid day name

                if (cur->has_weekly) {
                    cur->next_run = sched_calc_weekly(cur->weekday, cur->hour, cur->minute);
                }
            }
        }
        else if (strcmp(key, "datetime") == 0) {         // One-time absolute
            struct tm tm = {0};
            if (sscanf(val, "%d-%d-%d %d:%d:%d",
                     &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900;                      // Adjust for struct tm
                tm.tm_mon -= 1;                          // Month is 0-based
                cur->has_datetime = 1;
                cur->datetime_target = mktime(&tm);
                cur->next_run = cur->datetime_target;
            }
        }
        else if (strcmp(key, "api") == 0) {              // API call step
            if (sched_ensure_step_capacity(cur)) {
                cur->steps[cur->step_count].type = STEP_API;
                cs_strncpy(cur->steps[cur->step_count].data.api_query, val, MAX_QUERY);
                cur->step_count++;
            } else {
                cs_log("[%s] Step limit (%d) reached, 'api' step ignored", cur->name, MAX_STEPS);
            }
        }
        else if (strcmp(key, "log") == 0) {              // Log step
            if (sched_ensure_step_capacity(cur)) {
                cur->steps[cur->step_count].type = STEP_LOG;
                cs_strncpy(cur->steps[cur->step_count].data.log_msg, val, MAX_MSG);
                cur->step_count++;
            } else {
                cs_log("[%s] Step limit (%d) reached, 'log' step ignored", cur->name, MAX_STEPS);
            }
        }
        else if (strcmp(key, "sleep") == 0) {            // Sleep step
            int sec = atoi(val);
            if (sec > 0) {
                if (sched_ensure_step_capacity(cur)) {
                    cur->steps[cur->step_count].type = STEP_SLEEP;
                    cur->steps[cur->step_count].data.sleep_sec = sec;
                    cur->step_count++;
                } else {
                    cs_log("[%s] Step limit (%d) reached, 'sleep' step ignored", cur->name, MAX_STEPS);
                }
            }
        }
    }

    // Log loaded configuration for debugging
    int i;
    for (i = 0; i < g_sched.job_count; i++) {
        job_t *j = &g_sched.jobs[i];
        cs_log("Loaded job '%s': steps=%d loop=%d interval=%d daily=%d weekly=%d datetime=%d", 
               j->name, j->step_count, j->loop, j->interval_sec, 
               j->has_daily, j->has_weekly, j->has_datetime);
    }
    
    cs_log("Loaded %d job(s)", g_sched.job_count);
    return 0;                                            // Success
}
