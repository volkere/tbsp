/*
 * TBSP minimal C API: open interface, send/recv payload chunks.
 * Use for streaming or file transfer over Thunderbolt.
 */
#ifndef TBSP_API_H
#define TBSP_API_H

#include <stddef.h>

/* Safe for default MTU 1500 (frame = 14 + 40 + 1400). For Jumbo (MTU 9000) use 8192 and set ifconfig <if> mtu 9000. */
#define TBSP_API_MAX_PAYLOAD 1400

typedef struct tbsp_sender tbsp_sender_t;
typedef struct tbsp_receiver tbsp_receiver_t;

/* Sender: open interface (e.g. "en5"). Returns NULL on error. Requires root. */
tbsp_sender_t *tbsp_sender_open(const char *ifname);

/* Send one payload chunk (max TBSP_API_MAX_PAYLOAD bytes). Blocks until sent. Returns 0 on success, -1 on error. */
int tbsp_sender_send(tbsp_sender_t *s, const void *data, unsigned int len);

void tbsp_sender_close(tbsp_sender_t *s);

/* Receiver: open interface. Returns NULL on error. */
tbsp_receiver_t *tbsp_receiver_open(const char *ifname);

/* Receive one payload chunk. Returns payload length, 0 on timeout, -1 on error. Blocks up to timeout_ms. */
int tbsp_receiver_recv(tbsp_receiver_t *r, void *buf, unsigned int buf_size, int timeout_ms);

void tbsp_receiver_close(tbsp_receiver_t *r);

#endif
