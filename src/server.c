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

/*
 * Parse packet header, it is required at least the Fixed Header
 * of each packed, which is contained in the first 2 bytes in order
 * to read packet type and total lenght that we need to recv to 
 * complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming
 * streams of bytes and a structure formed by 2 fields.
 *
 * - buf -> a byte buffer, it will be malloc'ed in the function and
 *          it will contain the serialized bytes of the incoming packet.
 * - flags -> flags pointer, copy the flag setting of the incoming paclet,
 *            again for simplicity and convenience of the caller.
 */ 
static ssize_t recv_packet(int clientfd, unsigned char *buf, char *command)
{
  ssize_t nbytes = 0;

  /* Read the first byte, it should contain the message type code */
  if ((nbytes == recv_bytes(clientfd, buf, 1)) <= 0) {
    return -ERRCLIENTDC;
  }

  unsigned char byte = *buf;
  buf++;

  if (DISCONNECT < byte || CONNECT > byte) {
    return -ERRPACKETERR;
  }

  /* Read remaining lenght bytes which starts at byte 2 and ca be long to 
   * 4 bytes based on the size stored, so byte 2-5 is dedicated to the
   * packet lenght.
   */
  unsigned char buff[4];
  int count = 0;
  int n = 0;

  do {
    if ((n = recv_bytes(clientfd, buf+count, 1)) <= 0) {
      return -ERRCLIENTDC;
    }

    buff[count] = buf[count];
    nbytes += n;
  } while (buff[count++] & (1 << 7));

  // Reset temporary buffer
  const unsigned char *pbuf = &buff[0];
  unsigned long long tlen = mqtt_decode_lenght(&pbuf);

  /*
   * Set return code to -ERRMAXREQSIZE in case the total packet len
   * exceeds the configuration limit 'max_request_size'.
   */
  if (tlen > conf->max_request_size) {
    nbytes = -ERRMAXREQSIZE;
    goto exit;
  }

  /* Read remaining bytes to complete packet */
  if ((n = recv_bytes(clientfd, buf+1, tlen)) < 0) {
    goto err;
  }

  nbytes += n;
  *command = byte;

  exit:
    return nbytes;
  err:
    shutdown(clientfd, 0);
    close(clientfd);
    return nbytes;
}

/* Handle incoming request, after being accepted or after a reply */
static void on_read(struct evloop *loop, void *arg)
{
  struct closure *cb = arg;

  /* Raw bytes buffer to handle input from client */
  unsigned char *buffer = malloc(conf->max_request_size);
  ssize_t bytes = 0;
  char command = 0;

  /* 
   * We must read all incoming bytes untill an entire packet is
   * recieved. This is achieved by following the MQTT v3.1.1
   * protocol specifications, which send the size of the 
   * remaining packet as the second byte. By knowing it we know
   * if the packet is ready to be deserialized and used.
   */ 
  bytes = recv_packet(cb->fd, buffer, &command);

  /*
   * Looks like we got a client desconnection.
   *
   * TODO: Set a error handler for ERRMAXREQSIZE instead of 
   * dropping client connection, explicitly returning an informative
   * error code to the client connected.
   */
  if (bytes == -ERRCLIENTDC || bytes == -ERRMAXREQSIZE) {
    goto errdc;
  }

  info.bytes_recv++;

  /* 
   * Unpack recieved bytes into a mqtt_packet structure and 
   * execute the correct handler based on the type of the operation.
   */
  union mqtt_packet packet;
  unpack_mqtt_packet(buffer, &packet);
  union mqtt_header hdr = {
    .byte = command
  };

  /* Execute command callback */
  int rc = handlers[hdr.bits.type](cb, &packet);

  if (rc == REARM_W) {
    cb->call = on_write;

    /*
     * Reset handler to read_handler in order to read new
     * incomming data and EPOLL event for read fds.
     */
    evloop_rearm_callback_write(loop, cb);
  } else if (rc == REARM_R) {
    cb->call = on_read;
    evloop_rearm_callback_read(loop, cb);
  }

exit:
  free(buffer);
  return;
errdc:
  free(buffer);
  sol_error("Dropping client");
  shutdown(cb->fd, 0);
  close(cb->fd);
  hashtable_del(sol.clients, ((struct sol_client *) cb->obj)->client_id);
  hashtable_del(sol.closures, cb->closure_id);
  info.nclients--;
  info.nconnections--;
  return;
}

static void on_write(struct evloop *loop, void *arg)
{
  struct closure *cb = arg;
  ssize_t sent;
  if ((sent = send_bytes(cb->fd, cb->payload->data, cb->payload->size)) < 0) {
    sol_error("Error writing on socket to client %s: %s",
              ((struct sol_client *)cb->obj)->client_id, strerror(errno));

    /* Update information stats */
    info.bytes_sent += sent;
    bytestring_release(cb->payload);
    cb->payload = NULL;

    /* Re-arm callback by setting EPOLL event on EPOLLIN to read fds and
     * re-assinging the callback "on_read" for the next event.
     */
    cb->call = on_read;
    evloop_rearm_callback_read(loop, cb);
  }
}

/* 
 * Statistics topics, published every N seconds defined by configuration
 * interval.
 */
#define SYS_TOPICS      14

static const char *sys_topics[SYS_TOPICS] = {
  "$SOL/",
  "$SOL/broker/",
  "$SOL/broker/clients/",
  "$SOL/broker/bytes/",
  "$SOL/broker/messages/",
  "$SOL/broker/uptime/",
  "$SOL/broker/uptime/sol",
  "$SOL/broker/clients/connected/",
  "$SOL/broker/clients/disconnected/",
  "$SOL/broker/bytes/sent/",
  "$SOL/broker/bytes/received/",
  "$SOL/broker/messages/sent/",
  "$SOL/broker/messages/received/",
  "$SOL/broker/memory/used/",
};

static void run(struct evloop *loop)
{
  if (evloop_wait(loop) < 0) {
    sol_error("Event loop exited unexpectedely: %s", strerror(loop->status));
    evloop_free(loop);
  }
}

/*
 * Cleanup function to be passed in as a destructor to the hashtable for
 * connecting clients
 */ 
static int client_destructor(struct hashtable_entry *entry)
{
  if (!entry) {
    return -1;
  }

  struct sol_client *client = entry->val;

  if (client->client_id) {
    free(client->client_id);
  }

  free(client);
  return 0;
}

/*
 * Cleanup function to be passed in as a destructor to the hashtable for
 * registered closures.
 */ 
static int client_destructor(struct hashtable_entry *entry)
{
  if (!entry) {
    return -1;
  }

  struct closure *closure = entry->val;

  if (closure->payload) {
    bytestring_release(closure->payload);
  }

  free(closure);
  return 0;
}


