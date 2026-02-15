/* http.h
 * Embedded HTTP/1.1 server serving the dashboard and REST API.
 */

#ifndef HTTP_H
#define HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

void http_start(int port);
void http_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_H */
