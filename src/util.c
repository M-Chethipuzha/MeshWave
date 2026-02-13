/* util.c
 * Logging, time helpers, and socket utilities.
 */

#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/time.h>

void util_log(LogLevel level, const char *fmt, ...)
{
    const char *tag;
    switch (level) {
        case LOG_INFO:  tag = "INFO";  break;
        case LOG_WARN:  tag = "WARN";  break;
        case LOG_ERROR: tag = "ERROR"; break;
        default:        tag = "???";   break;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime(&tv.tv_sec);

    fprintf(stderr, "[%02d:%02d:%02d.%03ld] [%s] ",
            tm->tm_hour, tm->tm_min, tm->tm_sec,
            tv.tv_usec / 1000, tag);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}

long util_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

void util_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
