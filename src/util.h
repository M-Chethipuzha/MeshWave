/* util.h
 * Logging utilities and small helper functions.
 */

#ifndef UTIL_H
#define UTIL_H

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

#ifdef __cplusplus
extern "C" {
#endif

void util_log(LogLevel level, const char *fmt, ...);
long util_time_ms(void);
void util_set_nonblocking(int fd);

#ifdef __cplusplus
}
#endif

#endif /* UTIL_H */
