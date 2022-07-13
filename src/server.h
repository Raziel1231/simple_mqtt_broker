#ifndef SERVER_H
#define SERVER_H

/*
 * Epoll default settings for concurrent events monitored and timeout,
 * -1 means no timeout at all, blocking undefinitly
 */ 

#define EPOLL_MAX_EVENTS    256
#define EPOLL_TIMEOUT       -1

/*
 * Error codes for packet reception, signaling respectively:
 * - client disconnection.
 * - error reading packet.
 * - error packet sent exceeds size defined by configuration (generally
 *   default to 2MB)
 */

#define ERRCLIENTDC         1
#define ERRPACKETERR        2
#define ERRMAXREQSIZE       3

/*
 * Return code of handler functions, signaling if there is data payload
 * to be sent out or if the server just need to re-arm closure for reading
 * incomming bytes.
 */

#define REARM_R             0
#define REARM_W             1

int start_server(const char *, const char *);

#endif
