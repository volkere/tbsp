/*
 * Demo: receive a file over TBSP and save to disk.
 * Usage: sudo ./file_receiver <interface> [output_dir]
 * Example: sudo ./file_receiver en5 ./received
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "tbsp_api.h"

#define CHUNK TBSP_API_MAX_PAYLOAD

static int recv_file_header(tbsp_receiver_t *r, uint64_t *file_size, char *path, size_t path_max) {
    uint8_t buf[CHUNK];
    int n = tbsp_receiver_recv(r, buf, sizeof(buf), 30000);
    if (n < 12)
        return -1;
    *file_size = (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24)
        | ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) | ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
    uint32_t plen = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    if (plen >= path_max)
        plen = (uint32_t)(path_max - 1);
    if (n < (int)(12 + plen))
        return -1;
    memcpy(path, buf + 12, plen);
    path[plen] = '\0';
    return 0;
}

#define TBSP_DEFAULT_INTERFACE "en2"

int main(int argc, char **argv) {
    const char *ifname;
    const char *out_dir;
    if (argc >= 2) {
        ifname = argv[1];
        out_dir = argc > 2 ? argv[2] : ".";
    } else {
        ifname = TBSP_DEFAULT_INTERFACE;
        out_dir = ".";
    }

    tbsp_receiver_t *r = tbsp_receiver_open(ifname);
    if (!r) {
        fprintf(stderr, "tbsp_receiver_open failed (need root?)\n");
        return 1;
    }

    uint64_t file_size;
    char path[1025];
    fprintf(stderr, "Waiting for file header...\n");
    if (recv_file_header(r, &file_size, path, sizeof(path)) != 0) {
        fprintf(stderr, "recv header failed or timeout\n");
        tbsp_receiver_close(r);
        return 1;
    }

    char outpath[2048];
    snprintf(outpath, sizeof(outpath), "%s/%s", out_dir, path);
    FILE *f = fopen(outpath, "wb");
    if (!f) {
        fprintf(stderr, "fopen %s: %s\n", outpath, strerror(errno));
        tbsp_receiver_close(r);
        return 1;
    }

    uint8_t chunk[CHUNK];
    uint64_t received = 0;
    uint64_t last_progress = 0;
    while (received < file_size) {
        int n = tbsp_receiver_recv(r, chunk, sizeof(chunk), 5000);
        if (n < 0) {
            fprintf(stderr, "\nrecv error\n");
            break;
        }
        if (n == 0)
            continue;
        size_t to_write = (uint64_t)n > (file_size - received) ? (size_t)(file_size - received) : (size_t)n;
        if (fwrite(chunk, 1, to_write, f) != to_write) {
            perror("fwrite");
            break;
        }
        received += to_write;
        if (received - last_progress >= 100 * 1024 || received == file_size) {
            last_progress = received;
            fprintf(stderr, "\rReceived %llu / %llu bytes (%.0f%%)   ",
                (unsigned long long)received, (unsigned long long)file_size,
                file_size ? 100.0 * (double)received / (double)file_size : 0.0);
            fflush(stderr);
        }
    }

    fclose(f);
    tbsp_receiver_close(r);
    fprintf(stderr, "\nReceived %llu bytes -> %s\n", (unsigned long long)received, outpath);
    if (received == file_size)
        fprintf(stderr, "Transfer complete.\n");
    return received == file_size ? 0 : 1;
}
