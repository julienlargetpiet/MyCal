#define _POSIX_C_SOURCE 200809L

#include "upload.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAME_CAPACITY 128
#define PATH_CAPACITY 4096

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode the filename and keep only safe ASCII characters. */
static int sanitize_name(const char *encoded, char *out, size_t capacity)
{
    size_t j = 0;

    if (!encoded || !*encoded || capacity < 2)
        return -1;

    for (size_t i = 0; encoded[i]; i++) {
        unsigned char c = (unsigned char)encoded[i];

        if (c == '%') {
            if (!encoded[i + 1] || !encoded[i + 2])
                return -1;

            int a = hex_value((unsigned char)encoded[i + 1]);
            int b = hex_value((unsigned char)encoded[i + 2]);

            if (a < 0 || b < 0)
                return -1;

            c = (unsigned char)((a << 4) | b);
            i += 2;
        }

        /* Never interpret a filename as a path. */
        if (c == '/' || c == '\\' || c == '\0')
            return -1;

        if (c == ' ' || (c >= '\t' && c <= '\r'))
            c = '_';

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            continue;

        if (j == 0 && c == '.')
            continue;

        if (j + 1 >= capacity)
            return -1;

        out[j++] = (char)c;
    }

    out[j] = '\0';

    return j ? 0 : -1;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t written = write(fd, p, n);

        if (written < 0 && errno == EINTR)
            continue;

        if (written <= 0)
            return -1;

        p += written;
        n -= (size_t)written;
    }

    return 0;
}

static int valid_content(int fd, const char *name, size_t length)
{
    unsigned char header[16] = {0};

    ssize_t n = pread(fd, header, sizeof header, 0);

    if (n < 0)
        return 0;

    const char *ext = strrchr(name, '.');
    if (!ext)
        return 0;

    char lower[16];
    size_t extlen = strlen(ext);

    if (extlen >= sizeof lower)
        return 0;

    for (size_t i = 0; i <= extlen; i++)
        lower[i] = (char)tolower((unsigned char)ext[i]);

    if (!strcmp(lower, ".png"))
        return n >= 8 &&
               memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0;

    if (!strcmp(lower, ".jpg") || !strcmp(lower, ".jpeg"))
        return n >= 3 &&
               header[0] == 0xff &&
               header[1] == 0xd8 &&
               header[2] == 0xff;

    if (!strcmp(lower, ".gif"))
        return n >= 6 &&
               (!memcmp(header, "GIF87a", 6) ||
                !memcmp(header, "GIF89a", 6));

    if (!strcmp(lower, ".webp"))
        return n >= 12 &&
               !memcmp(header, "RIFF", 4) &&
               !memcmp(header + 8, "WEBP", 4);

    if (!strcmp(lower, ".pdf"))
        return n >= 5 && !memcmp(header, "%PDF-", 5);

    if (!strcmp(lower, ".mp4"))
        return n >= 12 &&
               !memcmp(header + 4, "ftyp", 4);

    if (!strcmp(lower, ".webm"))
        return n >= 4 &&
               !memcmp(header, "\x1a\x45\xdf\xa3", 4);

    /* For text documents, inspect the entire file. */
    if (!strcmp(lower, ".txt") ||
        !strcmp(lower, ".md") ||
        !strcmp(lower, ".csv")) {
        unsigned char buffer[8192];
        size_t position = 0;
        int utf8_remaining = 0;
        unsigned char min_next = 0x80, max_next = 0xbf;

        while (position < length) {
            size_t count = length - position;
            if (count > sizeof buffer)
                count = sizeof buffer;

            ssize_t got = pread(fd, buffer, count, (off_t)position);

            if (got <= 0)
                return 0;

            for (ssize_t i = 0; i < got; i++) {
                unsigned char c = buffer[i];

                if (c == 0)
                    return 0;

                if (utf8_remaining) {
                    if (c < min_next || c > max_next)
                        return 0;
                    utf8_remaining--;
                    min_next = 0x80;
                    max_next = 0xbf;
                } else if (c < 0x80) {
                    /* ASCII */
                } else if (c >= 0xc2 && c <= 0xdf) {
                    utf8_remaining = 1;
                } else if (c == 0xe0) {
                    utf8_remaining = 2;
                    min_next = 0xa0;
                } else if (c >= 0xe1 && c <= 0xec) {
                    utf8_remaining = 2;
                } else if (c == 0xed) {
                    utf8_remaining = 2;
                    max_next = 0x9f;
                } else if (c >= 0xee && c <= 0xef) {
                    utf8_remaining = 2;
                } else if (c == 0xf0) {
                    utf8_remaining = 3;
                    min_next = 0x90;
                } else if (c >= 0xf1 && c <= 0xf3) {
                    utf8_remaining = 3;
                } else if (c == 0xf4) {
                    utf8_remaining = 3;
                    max_next = 0x8f;
                } else {
                    return 0;
                }
            }

            position += (size_t)got;
        }

        return utf8_remaining == 0;
    }

    return 0;
}

int upload_file(int sock,
                const char *username,
                const char *upload_directory,
                const char *encoded_name,
                size_t length,
                const unsigned char *initial,
                size_t initial_len)
{
    char name[NAME_CAPACITY];

    if (length == 0 || length > MAX_UPLOAD_SIZE ||
        initial_len > length ||
        (initial_len > 0 && initial == NULL) ||
        sanitize_name(encoded_name, name, sizeof name) != 0)
        return -1;

    char temporary[PATH_CAPACITY];

    int n = snprintf(temporary, sizeof temporary,
                 "static/%s/%s/.uploadXXXXXX",
                 username, upload_directory);

    if (n < 0 || (size_t)n >= sizeof temporary)
        return -1;

    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;

    int failed = 0;

    if (initial_len &&
        write_all(fd, initial, initial_len) != 0)
        failed = 1;

    size_t received = initial_len;
    unsigned char buffer[16384];

    while (!failed && received < length) {
        size_t remaining = length - received;
        size_t want = remaining < sizeof buffer ?
                      remaining : sizeof buffer;

        ssize_t got = recv(sock, buffer, want, 0);

        if (got < 0 && errno == EINTR)
            continue;

        if (got <= 0 ||
            write_all(fd, buffer, (size_t)got) != 0) {
            failed = 1;
            break;
        }

        received += (size_t)got;
    }

    if (!failed && !valid_content(fd, name, length))
        failed = 1;

    if (!failed && fchmod(fd, 0644) != 0)
        failed = 1;

    if (!failed && fsync(fd) != 0)
        failed = 1;

    if (close(fd) != 0)
        failed = 1;

    if (failed) {
        unlink(temporary);
        return -1;
    }

    char final_path[PATH_CAPACITY];
    n = snprintf(final_path, sizeof final_path,
             "static/%s/%s/%s",
             username, upload_directory, name);

    if (n < 0 || (size_t)n >= sizeof final_path) {
        unlink(temporary);
        return -1;
    }
    
    /* Atomically replace the existing file, if any. */
    if (rename(temporary, final_path) != 0) {
        perror("rename");
        unlink(temporary);
        return -1;
    }
    
    return 0;
}



