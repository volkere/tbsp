/*
 * Variant A (RDMA-style) reference only – not used by Raw Ethernet TBSP.
 * Shared memory layout for DriverKit + user-space ring buffer.
 */
#pragma once
#include <stdint.h>

#define REGION_SIZE (16 * 1024 * 1024)
#define SLOT_SIZE   65536
#define NUM_SLOTS   (REGION_SIZE / SLOT_SIZE)

typedef struct __attribute__((aligned(64))) {
    volatile uint64_t producer_index;
    uint8_t pad1[56];
    volatile uint64_t consumer_index;
    uint8_t pad2[56];
    uint8_t data[REGION_SIZE];
} SharedRegion;

static inline uint64_t next_index(uint64_t index) {
    return (index + 1) % NUM_SLOTS;
}
