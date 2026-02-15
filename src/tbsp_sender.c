/*
 * TBSP minimal sender: blocking I/O, one frame at a time, sliding window.
 * Usage: sudo ./tbsp_sender <interface> [dest_mac]
 * Example: sudo ./tbsp_sender en5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/bpf.h>
#include <netinet/if_ether.h>
#include "tbsp_common.h"

#define ETH_HEADER_LEN 14
#define FRAME_BUF_SIZE (ETH_HEADER_LEN + sizeof(TBSPHeader) + PAYLOAD_SIZE)

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
            unsigned int blen = 4096;
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

static int send_data_frame(int bpf_fd, const uint8_t *dst_mac, const uint8_t *src_mac,
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

static int try_read_ack(int bpf_fd, uint8_t *buf, size_t buf_len) {
    ssize_t n = read(bpf_fd, buf, buf_len);
    if (n <= 0)
        return 0;
    char *ptr = (char *)buf;
    while (ptr + sizeof(struct bpf_hdr) <= (char *)buf + n) {
        struct bpf_hdr *bh = (struct bpf_hdr *)ptr;
        size_t hdrlen = bh->bh_hdrlen;
        size_t caplen = bh->bh_caplen;
        if (ptr + hdrlen + caplen > (char *)buf + n)
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
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <interface> [dest_mac]\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    /* Default: broadcast for local test; dest_mac can be parsed from argv[2] if needed */
    uint8_t dst_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t src_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

    int bpf_fd = open_bpf(ifname);
    if (bpf_fd < 0) {
        perror("open_bpf");
        return 1;
    }

    uint8_t read_buf[4096];
    uint8_t payload[PAYLOAD_SIZE];
    memset(payload, 0xAB, sizeof(payload));

    fprintf(stderr, "TBSP minimal sender on %s (broadcast). Send loop.\n", ifname);

    for (;;) {
        /* Flow control: only send if window allows */
        while (next_seq - last_acked >= remote_window) {
            if (try_read_ack(bpf_fd, read_buf, sizeof(read_buf)) <= 0)
                usleep(100);
        }

        size_t plen = PAYLOAD_SIZE;
        if (send_data_frame(bpf_fd, dst_mac, src_mac, payload, (uint32_t)plen) < 0) {
            perror("write");
            break;
        }

        try_read_ack(bpf_fd, read_buf, sizeof(read_buf));
        usleep(500);
    }

    close(bpf_fd);
    return 0;
}
