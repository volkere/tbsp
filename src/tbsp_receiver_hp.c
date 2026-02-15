/*
 * TBSP high-performance receiver: non-blocking, poll loop, ACK per frame.
 * Usage: sudo ./tbsp_receiver_hp <interface>
 * Example: sudo ./tbsp_receiver_hp en5
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
#define READ_BUF_SIZE  65536

static uint64_t expected_seq = 0;
static uint32_t free_slots = WINDOW_SIZE;

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

static void send_ack(int bpf_fd, const uint8_t *dst_mac, const uint8_t *src_mac) {
    uint8_t frame[ETH_HEADER_LEN + sizeof(TBSPHeader)];
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = (TBSP_ETHERTYPE >> 8) & 0xFF;
    frame[13] = TBSP_ETHERTYPE & 0xFF;

    TBSPHeader *h = (TBSPHeader *)(frame + ETH_HEADER_LEN);
    h->version = TBSP_VERSION;
    h->type = TBSP_TYPE_ACK;
    h->flags = 0;
    h->stream_id = 0;
    h->sequence = 0;
    h->ack = expected_seq;
    h->payload_len = 0;
    h->window = free_slots;
    h->timestamp_ns = now_ns();

    write(bpf_fd, frame, sizeof(frame));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    int bpf_fd = open_bpf(ifname);
    if (bpf_fd < 0) {
        perror("open_bpf");
        return 1;
    }

    uint8_t *read_buf = malloc(READ_BUF_SIZE);
    if (!read_buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    struct pollfd pfd = { .fd = bpf_fd, .events = POLLIN };
    fprintf(stderr, "TBSP HP receiver on %s. Poll loop.\n", ifname);

    for (;;) {
        int r = poll(&pfd, 1, 100);
        if (r < 0) {
            perror("poll");
            break;
        }
        if (r == 0)
            continue;

        ssize_t n = read(bpf_fd, read_buf, READ_BUF_SIZE);
        if (n <= 0)
            continue;

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

            if (th->type == TBSP_TYPE_DATA) {
                uint64_t seq = th->sequence;
                if (seq != expected_seq)
                    goto next;

                uint32_t plen = th->payload_len;
                if (plen > 0 && caplen >= ETH_HEADER_LEN + sizeof(TBSPHeader) + plen) {
                    uint8_t *payload = pkt + ETH_HEADER_LEN + sizeof(TBSPHeader);
                    fwrite(payload, 1, plen, stdout);
                }
                expected_seq++;
                free_slots = (free_slots > 0) ? free_slots - 1 : 0;

                uint8_t dst[6], src[6];
                memcpy(dst, pkt + 6, 6);
                memcpy(src, pkt, 6);
                send_ack(bpf_fd, dst, src);
                free_slots = WINDOW_SIZE;
            }

next:
            ptr += BPF_WORDALIGN(hdrlen + caplen);
        }
        fflush(stdout);
    }

    free(read_buf);
    close(bpf_fd);
    return 0;
}
