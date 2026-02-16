/*
 * TBSP C API implementation (BPF, sliding window).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/bpf.h>
#include <netinet/if_ether.h>
#include "tbsp_common.h"
#include "tbsp_api.h"

#define ETH_HEADER_LEN 14
/* Kernel BPF buffer; some systems cap this. Must hold >= WINDOW_SIZE frames (~1500 B each). */
#define READ_BUF_SIZE  (256 * 1024)
#define RECV_QUEUE_SIZE 2048

struct tbsp_sender {
    int bpf_fd;
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint64_t next_seq;
    uint64_t last_acked;
    uint32_t remote_window;
    uint8_t *read_buf;
    uint8_t *send_buf;
    uint32_t *send_lens;
};

struct tbsp_receiver {
    int bpf_fd;
    uint8_t sender_mac[6];
    uint8_t my_mac[6];
    uint64_t expected_seq;
    uint32_t free_slots;
    uint8_t *read_buf;
    uint8_t *queue_buf;
    uint32_t *queue_lens;
    int queue_head;
    int queue_tail;
};

static int open_bpf(const char *ifname) {
    char dev[32];
    int fd;
    for (int i = 0; i < 32; i++) {
        snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
        fd = open(dev, O_RDWR);
        if (fd >= 0) {
            unsigned int blen = READ_BUF_SIZE;
            if (ioctl(fd, BIOCSBLEN, &blen) < 0) {
                close(fd);
                return -1;
            }
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
                close(fd);
                return -1;
            }
            unsigned int imm = 1;
            ioctl(fd, BIOCIMMEDIATE, &imm);
            if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
                close(fd);
                return -1;
            }
            return fd;
        }
    }
    return -1;
}

tbsp_sender_t *tbsp_sender_open(const char *ifname) {
    int fd = open_bpf(ifname);
    if (fd < 0)
        return NULL;
    tbsp_sender_t *s = calloc(1, sizeof(tbsp_sender_t));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->bpf_fd = fd;
    memset(s->dst_mac, 0xFF, 6);
    s->src_mac[0] = 0x02;
    s->remote_window = WINDOW_SIZE;
    s->read_buf = malloc(READ_BUF_SIZE);
    s->send_buf = malloc((size_t)WINDOW_SIZE * TBSP_API_MAX_PAYLOAD);
    s->send_lens = malloc((size_t)WINDOW_SIZE * sizeof(uint32_t));
    if (!s->read_buf || !s->send_buf || !s->send_lens) {
        free(s->read_buf);
        free(s->send_buf);
        free(s->send_lens);
        free(s);
        close(fd);
        return NULL;
    }
    return s;
}

static void sender_drain_acks(tbsp_sender_t *s) {
    for (;;) {
        ssize_t n = read(s->bpf_fd, s->read_buf, READ_BUF_SIZE);
        if (n <= 0)
            return;
        char *ptr = (char *)s->read_buf;
        while (ptr + sizeof(struct bpf_hdr) <= (char *)s->read_buf + n) {
            struct bpf_hdr *bh = (struct bpf_hdr *)ptr;
            size_t hdrlen = bh->bh_hdrlen;
            size_t caplen = bh->bh_caplen;
            if (ptr + hdrlen + caplen > (char *)s->read_buf + n)
                break;
            uint8_t *pkt = (uint8_t *)(ptr + hdrlen);
            if (caplen >= ETH_HEADER_LEN + sizeof(TBSPHeader)) {
                uint16_t etype = (uint16_t)pkt[12] << 8 | pkt[13];
                if (etype == TBSP_ETHERTYPE) {
                    TBSPHeader *th = (TBSPHeader *)(pkt + ETH_HEADER_LEN);
                    if (th->type == TBSP_TYPE_ACK) {
                        if (th->ack > s->last_acked)
                            s->last_acked = th->ack;
                        s->remote_window = th->window;
                    }
                }
            }
            ptr += BPF_WORDALIGN(hdrlen + caplen);
        }
    }
}

static int send_one_frame_with_seq(tbsp_sender_t *s, uint64_t seq, const void *payload, unsigned int len) {
    size_t total = ETH_HEADER_LEN + sizeof(TBSPHeader) + len;
    uint8_t *frame = malloc(total);
    if (!frame)
        return -1;
    memcpy(frame, s->dst_mac, 6);
    memcpy(frame + 6, s->src_mac, 6);
    frame[12] = (TBSP_ETHERTYPE >> 8) & 0xFF;
    frame[13] = TBSP_ETHERTYPE & 0xFF;

    TBSPHeader *h = (TBSPHeader *)(frame + ETH_HEADER_LEN);
    h->version = TBSP_VERSION;
    h->type = TBSP_TYPE_DATA;
    h->flags = 0;
    h->stream_id = 0;
    h->sequence = seq;
    h->ack = s->last_acked;
    h->payload_len = len;
    h->window = s->remote_window;
    h->timestamp_ns = now_ns();
    memcpy(frame + ETH_HEADER_LEN + sizeof(TBSPHeader), payload, len);

    ssize_t n;
    int retries = 500;
    do {
        n = write(s->bpf_fd, frame, total);
        if (n == (ssize_t)total)
            break;
        if (n >= 0) {
            free(frame);
            return -1;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            break;
        usleep(1000);
    } while (--retries > 0);
    free(frame);
    return (n == (ssize_t)total) ? 0 : -1;
}

static int send_one_frame(tbsp_sender_t *s, const void *payload, unsigned int len) {
    uint64_t seq = s->next_seq;
    unsigned int idx = (unsigned int)((seq - (s->last_acked + 1)) % (uint64_t)WINDOW_SIZE);
    memcpy(s->send_buf + (size_t)idx * TBSP_API_MAX_PAYLOAD, payload, len);
    s->send_lens[idx] = len;

    if (send_one_frame_with_seq(s, seq, payload, len) != 0)
        return -1;
    s->next_seq++;
    return 0;
}

#define SENDER_ACK_TIMEOUT_MS  15000
#define SENDER_POLL_MS         50

int tbsp_sender_send(tbsp_sender_t *s, const void *data, unsigned int len) {
    if (len > TBSP_API_MAX_PAYLOAD)
        return -1;
    int wait_iters = SENDER_ACK_TIMEOUT_MS / SENDER_POLL_MS;
    uint64_t acked_at_start = s->last_acked;
    for (;;) {
        sender_drain_acks(s);
        if ((s->next_seq - s->last_acked) < s->remote_window)
            break;
        if (s->last_acked != acked_at_start) {
            acked_at_start = s->last_acked;
            wait_iters = SENDER_ACK_TIMEOUT_MS / SENDER_POLL_MS;
        }
        if (--wait_iters <= 0) {
            if (s->next_seq > s->last_acked + 1) {
                for (uint64_t seq = s->last_acked + 1; seq < s->next_seq; seq++) {
                    unsigned int idx = (unsigned int)((seq - (s->last_acked + 1)) % (uint64_t)WINDOW_SIZE);
                    const uint8_t *p = s->send_buf + (size_t)idx * TBSP_API_MAX_PAYLOAD;
                    uint32_t plen = s->send_lens[idx];
                    send_one_frame_with_seq(s, seq, p, plen);
                }
                wait_iters = SENDER_ACK_TIMEOUT_MS / SENDER_POLL_MS;
                continue;
            }
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = { .fd = s->bpf_fd, .events = POLLIN };
        if (poll(&pfd, 1, SENDER_POLL_MS) < 0)
            return -1;
    }
    return send_one_frame(s, data, len);
}

void tbsp_sender_close(tbsp_sender_t *s) {
    if (!s) return;
    if (s->bpf_fd >= 0) close(s->bpf_fd);
    free(s->read_buf);
    free(s->send_buf);
    free(s->send_lens);
    free(s);
}

tbsp_receiver_t *tbsp_receiver_open(const char *ifname) {
    int fd = open_bpf(ifname);
    if (fd < 0)
        return NULL;
    tbsp_receiver_t *r = calloc(1, sizeof(tbsp_receiver_t));
    if (!r) {
        close(fd);
        return NULL;
    }
    r->bpf_fd = fd;
    r->free_slots = WINDOW_SIZE;
    r->read_buf = malloc(READ_BUF_SIZE);
    r->queue_buf = malloc(RECV_QUEUE_SIZE * TBSP_API_MAX_PAYLOAD);
    r->queue_lens = malloc(RECV_QUEUE_SIZE * sizeof(uint32_t));
    if (!r->read_buf || !r->queue_buf || !r->queue_lens) {
        free(r->read_buf);
        free(r->queue_buf);
        free(r->queue_lens);
        free(r);
        close(fd);
        return NULL;
    }
    return r;
}

static void receiver_send_ack(tbsp_receiver_t *r) {
    uint8_t frame[ETH_HEADER_LEN + sizeof(TBSPHeader)];
    memcpy(frame, r->sender_mac, 6);
    memcpy(frame + 6, r->my_mac, 6);
    frame[12] = (TBSP_ETHERTYPE >> 8) & 0xFF;
    frame[13] = TBSP_ETHERTYPE & 0xFF;
    TBSPHeader *h = (TBSPHeader *)(frame + ETH_HEADER_LEN);
    h->version = TBSP_VERSION;
    h->type = TBSP_TYPE_ACK;
    h->flags = 0;
    h->stream_id = 0;
    h->sequence = 0;
    h->ack = r->expected_seq;
    h->payload_len = 0;
    h->window = r->free_slots;
    h->timestamp_ns = now_ns();
    write(r->bpf_fd, frame, sizeof(frame));
    r->free_slots = WINDOW_SIZE;
}

static int queue_push(tbsp_receiver_t *r, const void *payload, uint32_t plen) {
    int next = (r->queue_tail + 1) % RECV_QUEUE_SIZE;
    if (next == r->queue_head)
        return -1;
    if (plen > TBSP_API_MAX_PAYLOAD)
        plen = TBSP_API_MAX_PAYLOAD;
    memcpy(r->queue_buf + r->queue_tail * TBSP_API_MAX_PAYLOAD, payload, plen);
    r->queue_lens[r->queue_tail] = plen;
    r->queue_tail = next;
    return 0;
}

static int queue_pop(tbsp_receiver_t *r, void *buf, unsigned int buf_size) {
    if (r->queue_head == r->queue_tail)
        return -1;
    uint32_t plen = r->queue_lens[r->queue_head];
    if (plen > buf_size)
        plen = buf_size;
    memcpy(buf, r->queue_buf + r->queue_head * TBSP_API_MAX_PAYLOAD, plen);
    r->queue_head = (r->queue_head + 1) % RECV_QUEUE_SIZE;
    return (int)plen;
}

static void process_read_buffer(tbsp_receiver_t *r, ssize_t n) {
    char *ptr = (char *)r->read_buf;
    while (ptr + sizeof(struct bpf_hdr) <= (char *)r->read_buf + n) {
        struct bpf_hdr *bh = (struct bpf_hdr *)ptr;
        size_t hdrlen = bh->bh_hdrlen;
        size_t caplen = bh->bh_caplen;
        if (ptr + hdrlen + caplen > (char *)r->read_buf + n)
            break;
        uint8_t *pkt = (uint8_t *)(ptr + hdrlen);
        if (caplen < ETH_HEADER_LEN + sizeof(TBSPHeader))
            goto next;
        uint16_t etype = (uint16_t)pkt[12] << 8 | pkt[13];
        if (etype != TBSP_ETHERTYPE)
            goto next;
        TBSPHeader *th = (TBSPHeader *)(pkt + ETH_HEADER_LEN);
        if (th->type == TBSP_TYPE_DATA) {
            if (th->sequence < r->expected_seq) {
                memcpy(r->sender_mac, pkt + 6, 6);
                memcpy(r->my_mac, pkt, 6);
                receiver_send_ack(r);
                goto next;
            }
            if (th->sequence != r->expected_seq)
                goto next;
            uint32_t plen = th->payload_len;
            if (plen > 0 && caplen >= ETH_HEADER_LEN + sizeof(TBSPHeader) + plen) {
                const uint8_t *payload = pkt + ETH_HEADER_LEN + sizeof(TBSPHeader);
                if (queue_push(r, payload, plen) != 0)
                    return;
                memcpy(r->sender_mac, pkt + 6, 6);
                memcpy(r->my_mac, pkt, 6);
                r->expected_seq++;
                r->free_slots = r->free_slots > 0 ? r->free_slots - 1 : 0;
                receiver_send_ack(r);
            }
        }
next:
        ptr += BPF_WORDALIGN(hdrlen + caplen);
    }
}

static void drain_bpf_into_queue(tbsp_receiver_t *r) {
    struct pollfd pfd = { .fd = r->bpf_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0)
        return;
    ssize_t n;
    do {
        n = read(r->bpf_fd, r->read_buf, READ_BUF_SIZE);
        if (n <= 0)
            return;
        process_read_buffer(r, n);
    } while (n == (ssize_t)READ_BUF_SIZE);
}

int tbsp_receiver_recv(tbsp_receiver_t *r, void *buf, unsigned int buf_size, int timeout_ms) {
    if (r->queue_head != r->queue_tail) {
        drain_bpf_into_queue(r);
        if (r->queue_head != r->queue_tail)
            return queue_pop(r, buf, buf_size);
    }
    struct pollfd pfd = { .fd = r->bpf_fd, .events = POLLIN };
    int rv = poll(&pfd, 1, timeout_ms);
    if (rv <= 0)
        return rv == 0 ? 0 : -1;
    ssize_t n;
    do {
        n = read(r->bpf_fd, r->read_buf, READ_BUF_SIZE);
        if (n <= 0)
            return -1;
        process_read_buffer(r, n);
    } while (n == (ssize_t)READ_BUF_SIZE);
    if (r->queue_head != r->queue_tail)
        return queue_pop(r, buf, buf_size);
    return 0;
}

void tbsp_receiver_close(tbsp_receiver_t *r) {
    if (!r) return;
    if (r->bpf_fd >= 0) close(r->bpf_fd);
    free(r->read_buf);
    free(r->queue_buf);
    free(r->queue_lens);
    free(r);
}
