/*
 * log.c — Structured logging implementation.
 *
 * Output format:
 *   2026-10-28T14:35:01.123456  INFO  server.c:42  server started on port 5555
 */

#include "log.h"
#include <time.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static log_level_t s_level  = LOG_INFO;
static FILE       *s_stream = NULL;  /* NULL means use stderr */

/* -------------------------------------------------------------------------
 * log_init
 * ---------------------------------------------------------------------- */

void log_init(log_level_t level, FILE *stream) {
    s_level  = level;
    s_stream = stream;
}

void log_set_level(log_level_t level) {
    s_level = level;
}

/* -------------------------------------------------------------------------
 * log_write
 * ---------------------------------------------------------------------- */

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...) {
    if (level > s_level) return;

    FILE *out = s_stream ? s_stream : stderr;

    /* Timestamp with microsecond resolution */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_buf;
    gmtime_r(&ts.tv_sec, &tm_buf);

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    /* Level label */
    static const char *labels[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };
    const char *label = (level <= LOG_DEBUG) ? labels[level] : "?????";

    /* Strip leading path from __FILE__ for brevity */
    const char *base = file;
    const char *p;
    for (p = file; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    /* Format the user message */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(out, "%s.%06ld  %s  %s:%d  %s\n",
            tbuf, (long)(ts.tv_nsec / 1000L),
            label, base, line, msg);
    fflush(out);
}

/* -------------------------------------------------------------------------
 * Level helpers
 * ---------------------------------------------------------------------- */

int log_level_from_string(const char *s) {
    if (!s) return -1;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    if (strcasecmp(s, "warn")  == 0) return LOG_WARN;
    if (strcasecmp(s, "info")  == 0) return LOG_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    return -1;
}

const char *log_level_name(log_level_t level) {
    switch (level) {
    case LOG_ERROR: return "error";
    case LOG_WARN:  return "warn";
    case LOG_INFO:  return "info";
    case LOG_DEBUG: return "debug";
    default:        return "unknown";
    }
}
