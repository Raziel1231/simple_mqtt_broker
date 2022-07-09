#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt.h"
#include "pack.h"

static size_t unpack_mqtt_connect(const unsigned char *, union mqtt_header *,
                                  union mqtt_packet *);
static size_t unpack_mqtt_publish(const unsigned char *, union mqtt_header *,
                                  union mqtt_packet *);
static size_t unpack_mqtt_subscribe(const unsigned char *, union mqtt_header *,
                                    union mqtt_packet *);
static size_t unpack_mqtt_unsubscribe(const unsigned char *, union mqtt_header *, 
                                      union mqtt_packet *);
static size_t unpack_mqtt_ack(const unsigned char *, union mqtt_header *,
                              union mqtt_packet *);

static unsigned char *pack_mqtt_header(const union mqtt_header *);
static unsigned char *pack_mqtt_ack(const union mqtt_packet *);
static unsigned char *pack_mqtt_connack(const union mqtt_packet *);
static unsigned char *pack_mqtt_suback(const union mqtt_packet *);
static unsigned char *pack_mqtt_publish(const union mqtt_packet *);

/*
 * MQTT v3.1.1 standard, remaining lenght field on the fixed header
 * can be at most 4 bytes
 */

static const int MAX_LEN_BYTES = 4;

/*
 * Enconde remaining lenght on a MQTT packet header, comprised of 
 * variable Header and Payload if present. It does not take into 
 * account the bytes required to store itself. 
 */

int mqtt_encode_lenght(unsigned char *buf, size_t len)
{
    int bytes = 0;
    do 
    {
        if (bytes + 1 > MAX_LEN_BYTES) {
            return bytes;
        }

        short d = len % 128;
        len /= 128;
        
        /* If there are more digits to encode, set the top bit of 
         * this digit 
         */ 
        if (len > 0) {
            d |= 128;
        }

        buf[bytes++] = d;
    } while (len > 0);

    return bytes;
}

/*
 * Decode remaining lenght comprised of variable Header and Payload
 * if present. It does not take into account the bytes storing 
 * lenght.
 *
 * TODO Handle case where multiplier > 128 * 128 * 128
 */

unsigned long long mqtt_decode_lenght(const unsigned char **buf) 
{
  char c;
  int multiplier = 1;
  unsigned long long value = 0LL;
  do {
    c = (char)**buf;
    value += (c & 127) * multiplier;
    multiplier *= 128;
    (*buf)++;
  } while ((c & 128) != 0);

  return value;
}

/*
 * MQTT unpacking functions
 */

static size_t unpack_mqtt_connect(const unsigned char *buf,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) 
{
  struct mqtt_connect connect = {.header = *hdr};
  pkt->connect = connect;

  const unsigned char *init = buf;

  /*
   * Second byte of the fixed header, contains the lenght of the
   * remaining bytes of the connect packet
   */

  size_t len = mqtt_decode_lenght(&buf);

  /* For now ignore checks on protocol name and reserved bits,
   * just skip to the 8th byte
   */

  buf = init + 8;

  /* Read variable header byte flags */
  pkt->connect.byte = unpack_u8((const uint8_t **)&buf);

  /* Read keepalive MSB and LSB (2 bytes word) */
  pkt->connect.payload.keepalive = unpack_u16((const uint8_t **)&buf);

  /*Read CID lenght (2 bytes word) */
  uint16_t cid_len = unpack_u16((const uint8_t **)&buf);

  /* Read the client id */
  if (cid_len > 0) {
    pkt->connect.payload.client_id = malloc(cid_len + 1);
    unpack_bytes((const uint8_t **)&buf, cid_len,
                 pkt->connect.payload.client_id);
  }

  /* Read the will topic and message if will is set on flags */
  if (pkt->connect.bits.will == 1) {
    unpack_string16((unsigned char**)&buf, &pkt->connect.payload.will_topic);
    unpack_string16((unsigned char**)&buf, &pkt->connect.payload.will_message);
  }

  /* Read the username if username flag is set */
  if (pkt->connect.bits.username == 1) {
    unpack_string16((unsigned char**)&buf, &pkt->connect.payload.username);
  }

  /* Read the password if password flag is set */
  if (pkt->connect.bits.password == 1) {
    unpack_string16((unsigned char**)&buf, &pkt->connect.payload.password);
  }

  return len;
}

static size_t unpack_mqtt_publish(const unsigned char *buf,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) {
  struct mqtt_publish publish = {
      .header = *hdr
  };

  pkt->publish = publish;

  /*
   * Second byte of the fixed header, contains the lenght of remaining bytes
   * of the connect packet
   */ 
  size_t len = mqtt_decode_lenght(&buf);

  pkt->publish.topiclen =
      unpack_string16((unsigned char **)&buf, &pkt->publish.topic);
  uint16_t message_len = len;

  /* Read packet id */

  if (publish.header.bits.qos > AT_MOST_ONCE) {
    pkt->publish.pkt_id = unpack_u16((const uint8_t **)&buf);
    message_len -= sizeof(uint16_t);
  }

  /*
   * Message len is calculated substracting the lenght of the variable
   * header from the remaining lenght field that is in the Fixed header
   */ 

  message_len -= (sizeof(uint16_t) + pkt->publish.topiclen);
  pkt->publish.payloadlen = message_len;
  pkt->publish.payload = malloc(message_len + 1);
  unpack_bytes((const uint8_t **)&buf, message_len, pkt->publish.payload);
  
  return len;
}

static size_t unpack_mqtt_subscribe(const unsigned char *buf,
                                    union mqtt_header *hdr,
                                    union mqtt_packet *pkt) 
{
  struct mqtt_subscribe subscribe = {
    .header = *hdr
  };

  /*
   * Second byte of the fixed header, contains the lenght of the remaining
   * bytes of the connect packet
   */ 

  size_t len = mqtt_decode_lenght(&buf);
  size_t remaining_bytes = len;

  /* Read packet id */
  subscribe.pkt_id = unpack_u16((const uint8_t **)&buf);
  remaining_bytes -= sizeof(uint16_t);

  /*
   * Read in a loop all remaining bytes specified by len of the Fixed Header
   * From now on the payload consist of 3 tuples formed by:
   * - topic lenght
   * - topic filter (string)
   * - qos 
   */ 

  int i = 0;
  while (remaining_bytes > 0)
  {
    /* Read lenght bytes of the first topic filter */
    remaining_bytes -= sizeof(uint16_t);

    /* We have to make room for additional incoming tuples */
    subscribe.tuples = realloc(subscribe.tuples,
                               (i+1) * sizeof(*subscribe.tuples));
    subscribe.tuples[i].topic_len = 
        unpack_string16((unsigned char **)&buf, &subscribe.tuples[i].topic);
    remaining_bytes -= subscribe.tuples[i].topic_len;
    subscribe.tuples[i].qos = unpack_u8((const uint8_t **)&buf);

    len -= sizeof(uint8_t);
    i++;
  }

  subscribe.tuples_len = i;
  pkt->subscribe = subscribe;
  return len;
}

static size_t unpack_mqtt_unsubscribe(const unsigned char *buf,
                                      union mqtt_header *hdr,
                                      union mqtt_packet *pkt) 
{
  struct mqtt_unsuscribe unsubscribe = {
    .header = *hdr
  };

  /*
   * Second byte of the fixed header, contains the lenght of the remaining
   * bytes of the connect packet
   */ 

  size_t len = mqtt_decode_lenght(&buf);
  size_t remaining_bytes = len;

  /* Read packet id */
  unsubscribe.pkt_id = unpack_u16((const uint8_t **)&buf);
  remaining_bytes -= sizeof(uint16_t);

  /*
   * Read in a loop all remaining bytes specified by len of the Fixed Header
   * From now on the payload consist of 2 tuples formed by:
   * - topic lenght
   * - topic filter (string)
   */ 

  int i = 0;
  while (remaining_bytes > 0)
  {
    /* Read lenght bytes of the first topic filter */
    remaining_bytes -= sizeof(uint16_t);

    /* We have to make room for additional incoming tuples */
    unsubscribe.tuples = realloc(unsubscribe.tuples,
                               (i+1) * sizeof(*unsubscribe.tuples));
    unsubscribe.tuples[i].topic_len = 
        unpack_string16((unsigned char **)&buf, &unsubscribe.tuples[i].topic);
    remaining_bytes -= unsubscribe.tuples[i].topic_len;

    i++;
  }

  unsubscribe.tuples_len = i;
  pkt->unsuscribe = unsubscribe;
  return len;
}

static size_t unpack_mqtt_ack(const unsigned char *buf,
                              union mqtt_header *hdr,
                              union mqtt_packet *pkt)
{
  struct mqtt_ack ack = {
    .header = *hdr
  };

  /*
   * Second byte of the fixed header, contains the lenght of 
   * remaining bytes of the connect packet
   */

  size_t len = mqtt_decode_lenght(&buf);
  ack.pkt_id = unpack_u16((const uint8_t **)&buf);
  pkt->ack = ack;
  
  return len;
}

typedef size_t mqtt_unpack_handler (const unsigned char*,
                                    union mqtt_header *,
                                    union mqtt_packet *);

/* 
 * Unpack functions mapping unpack_handlers positioned in the array
 * based on message type
 */

static mqtt_unpack_handler *unpack_handlers[11] = {
  NULL,
  unpack_mqtt_connect,
  NULL,
  unpack_mqtt_publish,
  unpack_mqtt_ack,
  unpack_mqtt_ack,
  unpack_mqtt_ack,
  unpack_mqtt_ack,
  unpack_mqtt_subscribe,
  NULL,
  unpack_mqtt_unsubscribe
};

int unpack_mqtt_packet(const unsigned char *buf, union mqtt_packet *pkt)
{
  int rc = 0;

  /* Read first byte of the fixed header */
  unsigned char type = (unsigned char)*buf;
  union mqtt_header header = {
      .byte = type
  };

  if (header.bits.type == DISCONNECT 
      || header.bits.type == PINGREQ
      || header.bits.type == PINGRESP) {
    pkt->header = header;
  } else {
    /* Call the appropiate unpack handler based on the message type */
    rc = unpack_handlers[header.bits.type](++buf, &header, pkt);
  }

  return rc;
}

/*
 * MQTT packets building functions
 */ 

union mqtt_header *mqtt_packet_header (unsigned char byte)
{
  static union mqtt_header header;
  header.byte = byte;

  return &header;
}

struct mqtt_ack *mqtt_packet_ack (unsigned char byte, unsigned short pkt_id)
{
  static struct mqtt_ack ack;
  ack.header.byte = byte;
  ack.pkt_id = pkt_id;

  return &ack;
}

struct mqtt_connack *mqtt_packet_connack (unsigned char byte,
                                          unsigned char cflag,
                                          unsigned char rc)
{
  static struct mqtt_connack connack;
  connack.header.byte = byte;
  connack.byte = cflag;
  connack.rc = rc;

  return &connack;
}

struct mqtt_suback *mqtt_packet_suback (unsigned char byte,
                                        unsigned short pkt_id,
                                        unsigned char *rcs,
                                        unsigned short rcslen)
{
  struct mqtt_suback *suback = malloc(sizeof(*suback));
  suback->header.byte = byte;
  suback->pkt_id = pkt_id;
  suback->rcslen = rcslen;
  suback->rcs = malloc(rcslen);

  return suback;
}

struct mqtt_publish *mqtt_packet_publish (unsigned char byte,
                                          unsigned short pkt_id,
                                          size_t topiclen,
                                          unsigned char *topic,
                                          size_t payloadlen,
                                          unsigned char *payload)
{
  struct mqtt_publish *publish = malloc(sizeof(*publish));
  publish->header.byte = byte;
  publish->pkt_id = pkt_id;
  publish->topiclen = topiclen;
  publish->topic = topic;
  publish->payloadlen = payloadlen;
  publish->payload = payload;

  return publish;
}

void mqtt_packet_release(union mqtt_packet *pkt, unsigned type)
{
  switch (type) {
    case CONNECT:
      free(pkt->connect.payload.client_id);
      if (pkt->connect.bits.username == 1) {
        free(pkt->connect.payload.username);
      }
      if (pkt->connect.bits.password == 1) {
        free(pkt->connect.payload.password);
      }
      if (pkt->connect.bits.will == 1) {
        free(pkt->connect.payload.will_message);
        free(pkt->connect.payload.will_topic);
      }
      break;
    case SUBSCRIBE:
    case UNSUSCRIBE:
      for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++) {
        free(pkt->subscribe.tuples[i].topic);
      }
      free(pkt->subscribe.tuples);
      break;
    case PUBLISH:
      free(pkt->publish.topic);
      free(pkt->publish.payload);
      break;
    default:
      break;
  }
}
