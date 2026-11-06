#ifndef PSX3DS_STATE_H
#define PSX3DS_STATE_H

#include "psx3ds/types.h"

#define PSX_STATE_MAGIC    "PSX3DSS"
#define PSX_STATE_VERSION  1u
#define PSX_STATE_MAX_SIZE (6u * 1024u * 1024u)

#define PSX_FOURCC(a, b, c, d) \
    ((u32)(u8)(a) | ((u32)(u8)(b) << 8) | ((u32)(u8)(c) << 16) | ((u32)(u8)(d) << 24))

typedef struct StateIO {
    bool writing;
    bool error;
    u8  *buf;
    u32  pos;
    u32  cap;
} StateIO;

void state_u8(StateIO *s, u8 *v);
void state_u16(StateIO *s, u16 *v);
void state_u32(StateIO *s, u32 *v);
void state_u64(StateIO *s, u64 *v);
void state_bool(StateIO *s, bool *v);
void state_bytes(StateIO *s, void *data, u32 len);
void state_skip(StateIO *s, u32 len);

u32  psx_crc32(const void *data, size_t len);

#endif /* PSX3DS_STATE_H */
