#ifndef PACK_H
#define PACK_H

#include <stdio.h>
#include <stdint.h>
/* Reading data on const uint8_t pointer */

// bytes -> uint8_t
uint8_t unpack_u8(const uint8_t **);

// bytes -> uint16_t
uint16_t unpack_u16(const uint8_t **);

//bytes -> uint32_t
uint32_t unpack_u32(const uint8_t **);

// read a defined len of bytes
uint8_t *unpack_bytes(const uint8_t **, size_t, uint8_t *);

// unpack a string prefixed by its lenght as a uint16_t value
uint16_t unpack_string16(uint8_t **, uint8_t **);

/* Write data on const uint8_t pointer */

// append a uint8_t -> bytes into the bytestring
void pack_u8(uint8_t **, uint8_t);

// append a uint16_t -> bytes into the bytestring
void pack_u16(uint8_t **, uint16_t);

// append a uint32_t -> bytes into the bytestring
void pack_u32(uint8_t **, uint32_t);

// append len bytes into the bytestring
void pack_bytes(uint8_t **, uint8_t *);

/*
 * bytestring structure, provides a convenient way of handling byte string data.
 * It's essentially an unsigned char pointer that track the position of the
 * last written byte and the total size of the bytestring.
 */
struct bytestring {
  size_t size;
  size_t last;
  unsigned char *data;
};

/* 
 * Const struct bystring constructor, it require a size cause we use 
 * a bounded bytstring, e.g. no resize over a defined size.
 */
struct bytestring *bytestring_create(size_t);
void bytestring_init(struct bytestring*, size_t);
void bytestring_release(struct bytestring *);
void bytestring_reset(struct bytestring *);

#endif
