/*
 * TBSP high-performance sender: non-blocking, poll loop, batch send.
 * Usage: sudo ./tbsp_sender_hp <interface>
 * Example: sudo ./tbsp_sender_hp en5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/bpf.h>
#include <netinet/if_ether.h>
#include "tbsp_common.h"

#define ETH_HEADER_LEN 14
#define BATCH_SIZE     32
#define HP_PAYLOAD     8192
#define FRAME_BUF_SIZE (ETH_HEADER_LEN + sizeof(TBSPHeader) + HP_PAYLOAD)
#define READ_BUF_SIZE  65536

static uint64_t next_seq = 0;
static uint64_t last_acked = 0;
static uint32_t remote_window = WINDOW_SIZE;

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

static void build_eth_header(uint8_t *frame, const uint8_t *dst_mac, const uint8_t *src_mac) {
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = (TBSP_ETHERTYPE >> 8) & 0xFF;
    frame[13] = TBSP_ETHERTYPE & 0xFF;
}

static int send_one_data_frame(int bpf_fd, const uint8_t *dst_mac, const uint8_t *src_mac,
                               const void *payload, uint32_t len) {
    uint8_t frame[FRAME_BUF_SIZE];
    build_eth_header(frame, dst_mac, src_mac);

    TBSPHeader *h = (TBSPHeader *)(frame + ETH_HEADER_LEN);
    h->version = TBSP_VERSION;
    h->type = TBSP_TYPE_DATA;
    h->flags = 0;
    h->stream_id = 0;
    h->sequence = next_seq;
    h->ack = last_acked;
    h->payload_len = len;
    h->window = remote_window;
    h->timestamp_ns = now_ns();

    memcpy(frame + ETH_HEADER_LEN + sizeof(TBSPHeader), payload, len);
    size_t total = ETH_HEADER_LEN + sizeof(TBSPHeader) + len;

    ssize_t n = write(bpf_fd, frame, total);
    if (n != (ssize_t)total)
        return -1;
    next_seq++;
    return 0;
}

static void drain_acks(int bpf_fd, uint8_t *read_buf, size_t buf_len) {
    for (;;) {
        ssize_t n = read(bpf_fd, read_buf, buf_len);
        if (n <= 0)
            return;
        char *ptr = (char *)read_buf;
        while (ptr + sizeof(struct bpf_hdr) <= (char *)read_buf + n) {
            struct bpf_hdr *bh = (struct bpf_hdr *)ptr;
            size_t hdrlen = bh->bh_hdrlen;
            size_t caplen = bh->bh_caplen;
            if (ptr + hdrlen + caplen > (char *)read_buf + n)
                break;
            uint8_t *pkt = (uint8_t *)(ptr + hdrlen);
            if (caplen < ETH_HEADER_LEN + sizeof(TBSPHeader))
                goto next;
            uint16_t etype = (uint16_t)pkt[12] << 8 | pkt[13];
            if (etype != TBSP_ETHERTYPE)
                goto next;
            TBSPHeader *th = (TBSPHeader *)(pkt + ETH_HEADER_LEN);
            if (th->type == TBSP_TYPE_ACK) {
                if (th->ack > last_acked)
                    last_acked = th->ack;
                remote_window = th->window;
            }
next:
            ptr += BPF_WORDALIGN(hdrlen + caplen);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    uint8_t dst_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t src_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

    int bpf_fd = open_bpf(ifname);
    if (bpf_fd < 0) {
        perror("open_bpf");
        return 1;
    }

    uint8_t *read_buf = malloc(READ_BUF_SIZE);
    uint8_t *payload = malloc(HP_PAYLOAD);
    if (!read_buf || !payload) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    memset(payload, 0xAB, HP_PAYLOAD);

    struct pollfd pfd = { .fd = bpf_fd, .events = POLLIN };

    fprintf(stderr, "TBSP HP sender on %s (batch=%d, payload=%d).\n", ifname, BATCH_SIZE, HP_PAYLOAD);

    for (;;) {
        drain_acks(bpf_fd, read_buf, READ_BUF_SIZE);

        int sent = 0;
        while (sent < BATCH_SIZE && (next_seq - last_acked) < remote_window) {
            if (send_one_data_frame(bpf_fd, dst_mac, src_mac, payload, HP_PAYLOAD) < 0)
                break;
            sent++;
        }

        if (sent == 0 && (next_seq - last_acked) >= remote_window) {
            int r = poll(&pfd, 1, 10);
            if (r > 0)
                drain_acks(bpf_fd, read_buf, READ_BUF_SIZE);
            else if (r < 0) {
                perror("poll");
                break;
            }
        }
    }

    free(read_buf);
    free(payload);
    close(bpf_fd);
    return 0;
}
