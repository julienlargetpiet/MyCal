
#include <stddef.h>

#define MAX_UPLOAD_SIZE (25u * 1024u * 1024u)

/*
 * Returns 0 on success, -1 on failure.
 *
 * sock:          connected client socket
 * encoded_name:  URL-encoded original filename
 * length:        HTTP Content-Length
 * initial:       bytes already received after the HTTP headers
 * initial_len:   number of those bytes
 */
int upload_file(int sock,
                const char *username,
                const char *upload_directory,
                const char *encoded_name,
                size_t length,
                const unsigned char *initial,
                size_t initial_len);

