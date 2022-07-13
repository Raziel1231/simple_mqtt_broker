#include <netinet/in.h>
#define _POSIX_C_SOURCE     200809L
#include <time.h>
#include <errno.h>
#include <string.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "pack.h"
#include "util.h"
#include "mqtt.h"
//#include "core.h"
#include "network.h"
//#include "hashtable.h"
//#include "config.h"
#include "server.h"

/* Seconds in a Sol, easter egg */
static const double SOL_SECONDS = 88775.24;

/* 
 * General information of the broker, all fields will be published
 * periodically to internal topics
 */ 
static struct sol_info info;

/* Broker global instance, contains the topic trie and the clients hashtable */
static struct sol sol;

/* 
 * Prototype for a command handler, it accepts a pointer to the closure as the
 * link to the client sender of the command and a pointer to the packet itself
 */
typedef int handler(struct closure *, union mqtt_packet *);

/* Command handler, each one have responsability over a defined 
 * command packet
 */
static int connect_handler(struct closure *, union mqtt_packet *);
static int disconnect_handler(struct closure *, union mqtt_packet *);
static int subscribe_handler(struct closure *, union mqtt_packet *);
static int unsubscribe_handler(struct closure *, union mqtt_packet *);
static int publish_handler(struct closure *, union mqtt_packet *);
static int puback_handler(struct closure *, union mqtt_packet *);
static int pubrec_handler(struct closure *, union mqtt_packet *);
static int pubrel_handler(struct closure *, union mqtt_packet *);
static int pubcomp_handler(struct closure *, union mqtt_packet *);
static int pingreq_handler(struct closure *, union mqtt_packet *);

/* Command handler mapped using their position paired with their type */
static handler *handlers[15] = {
  NULL,
  connect_handler,
  NULL,
  publish_handler,
  puback_handler,
  pubrec_handler,
  pubrel_handler,
  pubcomp_handler,
  subscribe_handler,
  NULL,
  unsubscribe_handler,
  NULL,
  pingreq_handler,
  NULL,
  disconnect_handler
};

/* 
 * Connection structure for private use of the module, mainly for accepting
 * new connections
 */ 
struct connection {
  char ip[INET_ADDRSTRLEN + 1];
  int fd;
};

/* 
 * I/O closures, for the 3 main operation of the server:
 * - Accept a new connecting client
 * - Read incomming bytes from connected clients
 * - Write output bytes to connected clients
 */ 
static void on_read(struct evloop *, void *);
static void on_write(struct evloop *, void *);
static void on_accept(struct evloop *, void *);

/* 
 * Periodic task callback, will be executed every N seconds defined on 
 * the configuration.
 */ 
static int accept_new_client(int fd, struct connection *conn)
{
  if (!conn) {
    return -1;
  }

  /* Accept the connection */
  int clientsock = accept_connection(fd);
  
  /* Abort if not accepted */
  if (clientsock == -1) {
    return -1;
  }

  /* Just some informations retrieval of the new accepted client connection */
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  if (getpeername(clientsock, (struct sockaddr *)&addr, &addrlen) < 0) {
    return -1;
  }

  char ip_buff[INET_ADDRSTRLEN + 1];

  if (inet_ntop(AF_INET, &addr.sin_addr, ip_buff, sizeof(ip_buff)) == NULL) {
    return -1;
  }

  struct sockaddr_in sin;
  socklen_t sinlen = sizeof(sin);

  if (getsockname(fd, (struct sockaddr *)&sin, &sinlen) < 0) {
    return -1;
  }

  conn->fd = clientsock;
  strcpy(conn->ip, ip_buff);

  return 0;
}

/*
 * Handle new connection, create a fresh new struct client structure and
 * link it to the fd, ready to be set in EPOLLIN event
 */
static void on_accept(struct evloop *loop, void *arg)
{
  /* struct connection *server_conn = arg */
  struct closure *server = arg;
  struct connection conn;

  accept_new_client(server->fd, &conn);
  /* Create a client structure to handle his context connection */
  struct closure *client_closure = malloc(sizeof(*client_closure));

  if (!client_closure) {
    return;
  }

  /* Populate client structure */
  client_closure->fd = conn.fd;
  client_closure->obj = NULL;
  client_closure->payload = NULL;
  client_closure->arg = client_closure;
  client_closure->call = on_read;
  generate_uuid(client_closure->closure_id);
  hashtable_put(sol.closures, client_closure->closure_id, client_closure);

  /* Add it to the epoll loop */
  evloop_add_callback(loop, client_closure);

  /* Rearm server fd to accept new connections */
  evloop_rearm_callback_read(loop, server);

  /* Record the new client connected */
  info.nclients++;
  info.nconnections++;
  sol_info("New connection from %s on port %s", conn.ip, conf->port);
}


