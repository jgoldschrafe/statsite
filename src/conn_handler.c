#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <regex.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>
#include "metrics.h"
#include "streaming.h"
#include "conn_handler.h"

/* Static method declarations */
static int buffer_after_terminator(char *buf, int buf_len, char terminator, char **after_term, int *after_len);

/* These are the quantiles we track */
static double QUANTILES[] = {0.5, 0.95, 0.99};
static int NUM_QUANTILES = 3;

/**
 * This is the current metrics object we are using
 */
static metrics *GLOBAL_METRICS;
static statsite_config *GLOBAL_CONFIG;

/**
 * Invoked to initialize the conn handler layer.
 */
void init_conn_handler(statsite_config *config) {
    // Make the initial metrics object
    metrics *m = malloc(sizeof(metrics));
    int res = init_metrics(config->timer_eps, (double*)&QUANTILES, NUM_QUANTILES, m);
    assert(res == 0);
    GLOBAL_METRICS = m;

    // Store the config
    GLOBAL_CONFIG = config;
}

/**
 * Streaming callback to format our output
 */
static int stream_formatter(FILE *pipe, void *data, metric_type type, char *name, void *value) {
    #define STREAM(...) if (fprintf(pipe, __VA_ARGS__, (long long)tv->tv_sec) < 0) return 1;
    struct timeval *tv = data;
    switch (type) {
        case KEY_VAL:
            STREAM("kv.%s|%f|%lld\n", name, *(double*)value);
            break;

        case COUNTER:
            STREAM("counts.%s|%f|%lld\n", name, counter_sum(value));
            break;

        case TIMER:
            STREAM("timers.%s.sum|%f|%lld\n", name, timer_sum(value));
            STREAM("timers.%s.mean|%f|%lld\n", name, timer_mean(value));
            STREAM("timers.%s.lower|%f|%lld\n", name, timer_min(value));
            STREAM("timers.%s.upper|%f|%lld\n", name, timer_max(value));
            STREAM("timers.%s.count|%lld|%lld\n", name, timer_count(value));
            STREAM("timers.%s.stdev|%f|%lld\n", name, timer_stddev(value));
            STREAM("timers.%s.median|%f|%lld\n", name, timer_query(value, 0.5));
            STREAM("timers.%s.p95|%f|%lld\n", name, timer_query(value, 0.95));
            STREAM("timers.%s.p99|%f|%lld\n", name, timer_query(value, 0.99));
            break;

        default:
            syslog(LOG_ERR, "Unknown metric type: %d", type);
            break;
    }
    return 0;
}

/**
 * This is the thread that is invoked to handle flushing metrics
 */
static void* flush_thread(void *arg) {
    // Cast the args
    metrics *m = arg;

    // Get the current time
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Stream the records
    int res = stream_to_command(m, &tv, stream_formatter, GLOBAL_CONFIG->stream_cmd);
    if (res != 0) {
        syslog(LOG_WARNING, "Streaming command exited with status %d", res);
    }

    // Cleanup
    destroy_metrics(m);
    free(m);
}

/**
 * Invoked to when we've reached the flush interval timeout
 */
void flush_interval_trigger() {
    // Make a new metrics object
    metrics *m = malloc(sizeof(metrics));
    init_metrics(GLOBAL_METRICS->eps, (double*)&QUANTILES, NUM_QUANTILES, m);

    // Swap with the new one
    metrics *old = GLOBAL_METRICS;
    GLOBAL_METRICS = m;

    // Start a flush thread
    pthread_t thread;
    pthread_create(&thread, NULL, flush_thread, old);
    pthread_detach(thread);
}

/**
 * Called when statsite is terminating to flush the
 * final set of metrics
 */
void final_flush() {
    // Get the last set of metrics
    metrics *old = GLOBAL_METRICS;
    GLOBAL_METRICS = NULL;

    // Start a flush thread
    pthread_t thread;
    pthread_create(&thread, NULL, flush_thread, old);

    // Wait for the thread to finish
    pthread_join(thread, NULL);
}

/**
 * Invoked by the networking layer when there is new
 * data to be handled. The connection handler should
 * consume all the input possible, and generate responses
 * to all requests.
 * @arg handle The connection related information
 * @return 0 on success.
 */
int handle_client_connect(statsite_conn_handler *handle) {
    // Look for the next command line
    char *buf, *key, *val_str, *type_str, *endptr;
    metric_type type;
    int buf_len, should_free, status, i, after_len;
    double val;
    while (1) {
        status = extract_to_terminator(handle->conn, '\n', &buf, &buf_len, &should_free);
        if (status == -1) return 0; // Return if no command is available

        // Check for a valid metric
        // Scan for the colon
        status = buffer_after_terminator(buf, buf_len, ':', &val_str, &after_len);
        if (!status) status |= buffer_after_terminator(val_str, after_len, '|', &type_str, &after_len);
        if (status == 0) {
            // Convert the type
            switch (*type_str) {
                case 'c':
                    type = COUNTER;
                    break;
                case 'm':
                    type = TIMER;
                    break;
                case 'k':
                    type = KEY_VAL;
                    break;
                default:
                    type = UNKNOWN;
            }

            // Convert the value to a double
            endptr = NULL;
            val = strtod(val_str, &endptr);

            // Store the sample if we did the conversion
            if (val != 0 || endptr != val_str) {
                metrics_add_sample(GLOBAL_METRICS, type, buf, val);
            } else {
                syslog(LOG_WARNING, "Failed value conversion! Input: %s", val_str);
            }
        } else {
            syslog(LOG_WARNING, "Failed parse metric! Input: %s", buf);
        }

        // Make sure to free the command buffer if we need to
        if (should_free) free(buf);
    }

    return 0;
}


/**
 * Scans the input buffer of a given length up to a terminator.
 * Then sets the start of the buffer after the terminator including
 * the length of the after buffer.
 * @arg buf The input buffer
 * @arg buf_len The length of the input buffer
 * @arg terminator The terminator to scan to. Replaced with the null terminator.
 * @arg after_term Output. Set to the byte after the terminator.
 * @arg after_len Output. Set to the length of the output buffer.
 * @return 0 if terminator found. -1 otherwise.
 */
static int buffer_after_terminator(char *buf, int buf_len, char terminator, char **after_term, int *after_len) {
    // Scan for a space
    char *term_addr = memchr(buf, terminator, buf_len);
    if (!term_addr) {
        *after_term = NULL;
        return -1;
    }

    // Convert the space to a null-seperator
    *term_addr = '\0';

    // Provide the arg buffer, and arg_len
    *after_term = term_addr+1;
    *after_len = buf_len - (term_addr - buf + 1);
    return 0;
}

