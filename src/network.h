#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include "util.h"

/* Socket families */
#define UNIX    0
#define INET    1

/* Set non-blocking socket */
int set_nonblocking(int);

/*
 * Set TCP_NODELAY flag to true, desabling Nagle's algorithm, no more
 * waiting for incoming packets on the buffer
 */

int set_tcp_nodelay(int);

/* Auxiliary function for creating epoll server */
int create_and_bind(const char *, const char *, int);

/* Create a non-blocking socket and make it listen on the specified
 * adress and port
 */
int make_listen(const char *, const char *, int);

/* Accept a connection and add it to the right epollfd */
int accept_connection(int);

/* Helper functions to create and find socket to listen for new connections
 * and to set socket in non-blocking mode.
 */



/* I/O managment functions */

/* 
 * Send all data in a loop, avoiding interruption based on the kernel 
 * buffer availability. Used to send all bytes out at once while loop 
 * untill no bytes left. By handling EAGAIN and EWOULDBLOCK error codes.
 */
ssize_t send_bytes(int, const unsigned char *, size_t);

/*
 * Recieve (read) an arbitrary number of bytes from a descriptor and 
 * store them in a buffer, again handling correctly EAGAIN and 
 * EWOULDBLOCK error codes
 */
ssize_t recv_bytes(int, unsigned char *, size_t);

/*
 * Event loop wrapper structure, define and EPOLL loop and his status. The 
 * EPPOL instance use EPOLLNESHOT for each event and must be re-armed 
 * manually, in order to allow future uses on a multithreaded architecture.
 */

struct evloop {
  int epollfd;
  int max_events;
  int timeout;
  int status;
  struct epoll_event *events;
  /* Dynamic array of periodic task, a pair descriptor - closure */
  int periodic_maxsize;
  int periodic_nr;

  struct {
    int timerfd;
    struct closure *closure;
  } **periodic_task;
};

typedef void callback(struct evloop *, void *);

/* Callback object, represents a callback function with an associated
 * descriptor if needed, args is a void pointer wich can be a structure
 * poiting to callback parameters and closure_id a UUID for the closure 
 * itsefl. 
 * The last two fields are payload, a serialized version of the result of a
 * callback, ready to be sent through wire and a function pointer to the 
 * callback function execute.
 */

struct closure {
  int fd;
  void *obj;
  void *arg;
  char closure_id[UUID_LEN];
  struct bytestring *payload;
  callback *call;
};

struct evloop *evloop_create(int, int);

void evloop_init(struct evloop *, int, int);
void evloop_free(struct evloop *);

/*
 * Blocks in a while awaiting for events to be raised on monitored
 * file descriptors and executing the paired callback previously
 * registered
 */
int evloop_wait(struct evloop *);

/* 
 * Register a clorsure with a function to be executed every time the paired
 * descriptor is re-armed
 */
void evloop_add_callback(struct evloop *, struct closure *);

/*
 * Register a periodic clusure with a function to be executed every defined
 * interval of time.
 */ 
void evloop_add_periodic_task(struct evloop *, int, unsigned long long, 
                              struct closure *);

/* 
 * Unregister a closure by removing the associated descriptor from the 
 * EPOLL loop.
 */
int evloop_del_callback(struct evloop *, struct closure *);

/* 
 * Rearm the file descriptor associated with a closure for read action,
 * making the event loop to monitor the callback for reading events.
 */
int evloop_rearm_callback_read(struct evloop *, struct closure *);

/* 
 * Rearm the file descriptor associated with a closure for write action,
 * making the event loop to monitor the callback for writing events.
 */
int evloop_rearm_callback_write(struct evloop *, struct closure *);

/* Epool managment functions */
int epoll_add(int, int, int, void *);

/*
 * Modify an epoll-monitored descriptor, automatically set EPOLLONESHOT
 * in addition to the other flags, which can be EPOLLIN for read and 
 * EPOLLOUT for write
 */ 
int epoll_mod(int, int, int, void *);

/*
 * Remove a descriptor from an epoll descriptor, making it no-longer
 * monitored for events
 */
int epoll_del(int, int);


#endif
