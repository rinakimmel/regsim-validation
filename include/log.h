/*
 * log.h — Structured logging with levels and ISO-8601 timestamps.
 *
 * Usage:
 *   log_init(LOG_DEBUG, stderr);
 *   log_info("server started on port %d", port);
 *   log_debug("received %zu bytes from fd %d", n, fd);
 */

#ifndef REGSIM_LOG_H
#define REGSIM_LOG_H

#include <stdio.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Log levels (increasing verbosity)
 * ---------------------------------------------------------------------- */

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
} log_level_t;

/* -------------------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------------- */

/*
 * log_init — set the minimum level and the output stream.
 * Thread-safety: not guaranteed; call once from main before any threads.
 * Default before calling log_init: level=LOG_INFO, stream=stderr.
 */
void log_init(log_level_t level, FILE *stream);

/*
 * log_set_level — change the minimum level at runtime.
 */
void log_set_level(log_level_t level);

/* -------------------------------------------------------------------------
 * Logging functions
 * ---------------------------------------------------------------------- */

/*
 * log_write — low-level emit; prefer the macros below.
 * `file` and `line` are used in DEBUG output.
 */
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Convenience macros that capture __FILE__ and __LINE__ */
#define log_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

/* -------------------------------------------------------------------------
 * Level name helpers
 * ---------------------------------------------------------------------- */

/* Parse "error"/"warn"/"info"/"debug" (case-insensitive) into log_level_t.
 * Returns -1 on unrecognised string. */
int log_level_from_string(const char *s);

/* Return the canonical name string for a level. */
const char *log_level_name(log_level_t level);

#endif /* REGSIM_LOG_H */
