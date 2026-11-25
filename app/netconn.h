/*
 * netconn: network use per connection.
 *
 * Linux does not count network traffic per process, and an app can't see which
 * process owns another user's socket. It can ask the kernel (sock_diag) for every
 * TCP connection's byte counters, so traffic is shown per service and other end:
 * "Video (RTSP) · 192.0.2.12". UDP (some video streams) has no such counters.
 */
#ifndef NETCONN_H
#define NETCONN_H

#include <glib.h>

typedef struct {
	char service[32];      /* "Video (RTSP)", "Web (HTTPS)", "To Web (HTTPS)", "Port 9000" */
	char peer[48];         /* the other end's address */
	double tx, rx;         /* Mbit/s since the previous scan */
	int conns;
} NetUse;

/* Every TCP connection outside the camera, added together by service and other end,
 * busiest first. NULL if the camera does not allow the query. Call from one thread only. */
GArray* netconn_scan(void);

#endif
