/*
 * Demo: send a file over TBSP.
 * Usage: sudo ./file_sender <interface> <file> [remote_name]
 * Example: sudo ./file_sender en5 /path/to/video.mp4
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "tbsp_api.h"

#define CHUNK TBSP_API_MAX_PAYLOAD

/* First frame: 8 byte size (LE) + 4 byte path_len (LE) + path bytes */
static int send_file_header(tbsp_sender_t *s, uint64_t file_size, const char *path) {
    uint32_t plen = (uint32_t)strlen(path);
    if (plen > 1024)
        plen = 1024;
    size_t hdr_size = 8 + 4 + plen;
    uint8_t buf[8 + 4 + 1024];
    buf[0] = (uint8_t)(file_size);
    buf[1] = (uint8_t)(file_size >> 8);
    buf[2] = (uint8_t)(file_size >> 16);
    buf[3] = (uint8_t)(file_size >> 24);
    buf[4] = (uint8_t)(file_size >> 32);
    buf[5] = (uint8_t)(file_size >> 40);
    buf[6] = (uint8_t)(file_size >> 48);
    buf[7] = (uint8_t)(file_size >> 56);
    buf[8] = (uint8_t)(plen);
    buf[9] = (uint8_t)(plen >> 8);
    buf[10] = (uint8_t)(plen >> 16);
    buf[11] = (uint8_t)(plen >> 24);
    memcpy(buf + 12, path, plen);
    return tbsp_sender_send(s, buf, (unsigned int)hdr_size);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <interface> <file> [remote_name]\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];
    const char *filepath = argv[2];
    const char *remote_name = argc > 3 ? argv[3] : filepath;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        perror(filepath);
        return 1;
    }
    if (fseeko(f, 0, SEEK_END) != 0) {
        perror("fseeko");
        fclose(f);
        return 1;
    }
    uint64_t file_size = (uint64_t)ftello(f);
    rewind(f);

    tbsp_sender_t *s = tbsp_sender_open(ifname);
    if (!s) {
        fprintf(stderr, "tbsp_sender_open failed (need root?)\n");
        fclose(f);
        return 1;
    }

    if (send_file_header(s, file_size, remote_name) != 0) {
        perror("send header failed");
        tbsp_sender_close(s);
        fclose(f);
        return 1;
    }

    uint8_t chunk[CHUNK];
    uint64_t sent = 0;
    uint64_t last_progress = 0;
    while (sent < file_size) {
        size_t to_read = (file_size - sent) > CHUNK ? CHUNK : (size_t)(file_size - sent);
        size_t n = fread(chunk, 1, to_read, f);
        if (n == 0)
            break;
        if (tbsp_sender_send(s, chunk, (unsigned int)n) != 0) {
            perror("send chunk failed");
            break;
        }
        sent += n;
        if (sent - last_progress >= 100 * 1024 || sent == file_size) {
            last_progress = sent;
            fprintf(stderr, "\rSent %llu / %llu bytes (%.0f%%)   ",
                (unsigned long long)sent, (unsigned long long)file_size,
                file_size ? 100.0 * (double)sent / (double)file_size : 0.0);
            fflush(stderr);
        }
        if (sent == file_size) {
            fprintf(stderr, "\nTransfer complete.\n");
            fflush(stderr);
        }
    }

    tbsp_sender_close(s);
    fclose(f);
    fprintf(stderr, "Sent %llu of %llu bytes\n", (unsigned long long)sent, (unsigned long long)file_size);
    fflush(stderr);
    return sent == file_size ? 0 : 1;
}
