#pragma once
#include <stdint.h>
#include <time.h>

#define TBSP_ETHERTYPE 0x88B5
#define TBSP_VERSION   1
#define MAX_FRAME      9000
/* Max in-flight frames; keep small so BPF buffer never overflows (64 frames = ~96 KB). */
#define WINDOW_SIZE    64
#define PAYLOAD_SIZE   1400

/* Frame types */
#define TBSP_TYPE_DATA 0
#define TBSP_TYPE_ACK  1

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;   /* 0=DATA, 1=ACK */
    uint16_t flags;
    uint32_t stream_id;
    uint64_t sequence;
    uint64_t ack;
    uint32_t payload_len;
    uint32_t window;
    uint64_t timestamp_ns;
} TBSPHeader;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
}
