#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "pack.h"

/* Reading data */
uint8_t unpack_u8(const uint8_t ** buf)
{
    uint8_t val = **buf;
    (*buf)++;
    return val;
}

uint16_t unpack_u16(const uint8_t **buf)
{
    uint16_t val;
    memcpy(&val, *buf, sizeof(uint16_t));
    (*buf) += sizeof(uint16_t);

    return ntohs(val);
}

uint32_t unpack_u32(const uint8_t **buf)
{
    uint32_t val;
    memcpy(&val, *buf, sizeof(uint32_t));
    (*buf) += sizeof(uint32_t);

    return ntohl(val);
}

uint8_t* unpack_bytes(const uint8_t **buf, size_t len, uint8_t *str)
{
    memcpy(str, *buf, len);
    str[len] = '\0';
    (*buf) += len;
    return str;
}

uint16_t unpack_string16(uint8_t **buf, uint8_t **dest)
{
    uint16_t len = unpack_u16((const uint8_t**)buf);
    *dest = malloc(len + 1);
    *dest = unpack_bytes((const uint8_t **)buf, len, *dest);
    return len;
}

void pack_u8(uint8_t ** buf, uint8_t val)
{
    **buf = val;
    (*buf) += sizeof(uint8_t);
}

void pack_u16(uint8_t **buf, uint16_t val)
{
    uint16_t htonsval = htons(val);
    memcpy(*buf, &htonsval, sizeof(uint16_t));
    (*buf) += sizeof(uint16_t);
}

void pack_u32(uint8_t **buf, uint32_t val)
{
    uint32_t htonlval = htonl(val);
    memcpy(*buf, &htonlval , sizeof(uint32_t));
    (*buf) += sizeof(uint32_t);
}

void pack_bytes(uint8_t **buf, uint8_t *str)
{
    size_t len = strlen((char *)str);
    memcpy(*buf, str, len);
    (*buf) += len;
}

struct bytestring *bytestring_create(size_t len)
{
  struct bytestring *bstring = malloc(sizeof(*bstring));
  bytestring_init(bstring, len);
  return bstring;
}

void bytestring_init(struct bytestring *bstring, size_t size)
{
  if (!bstring) {
    return;
  }

  bstring->size = size;
  bstring->data = malloc(sizeof(unsigned char) * size);
  bytestring_reset(bstring);
}

void bytestring_release(struct bytestring *bstring)
{
  if (!bstring) {
    return;
  }

  free(bstring->data);
  free(bstring);
}

void bytestring_reset(struct bytestring *bstring)
{
  if (!bstring) {
    return;
  }

  bstring->data = 0;
  memset(bstring->data, 0, bstring->size);
}
