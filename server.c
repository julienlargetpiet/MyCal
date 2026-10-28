#define _POSIX_C_SOURCE 200809L

#include <sodium.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "upload.h"
#include <sys/random.h>
#include <mysql.h>
#include <dirent.h>

#define PORT 18081
#define MAX_REQUEST 16384
#define MAX_HEADER_SIZE 8000
#define MAX_LOGIN_SIZE 4096
#define TIMEOUT_SECONDS 5
#define PATH_CAPACITY 2048

static pthread_mutex_t csv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t upload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int client;
    MYSQL *db;
} ClientArgs;

int generate_token(char token[65])
{
    unsigned char random_bytes[32];
    static const char hex[] = "0123456789abcdef";

    size_t received = 0;
    
    while (received < sizeof random_bytes) {
        ssize_t n = getrandom(
            random_bytes + received,
            sizeof random_bytes - received,
            0
        );
    
        if (n < 0 && errno == EINTR)
            continue;
    
        if (n <= 0)
            return -1;
    
        received += (size_t)n;
    }

    for (size_t i = 0; i < sizeof random_bytes; i++) {
        token[i * 2]     = hex[random_bytes[i] >> 4];
        token[i * 2 + 1] = hex[random_bytes[i] & 15];
    }

    token[64] = '\0';
    return 0;
}

static int retrieve_token(const char *cookies, char token[65])
{
    const char *p = cookies;
    const char *key = "__Host-mycal_session=";
    size_t keylen = strlen(key);

    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ';')
            p++;

        const char *end = strchr(p, ';');
        if (!end)
            end = p + strlen(p);

        if ((size_t)(end - p) == keylen + 64 &&
            strncmp(p, key, keylen) == 0) {

            const char *value = p + keylen;

            for (size_t i = 0; i < 64; i++) {
                if (!((value[i] >= '0' && value[i] <= '9') ||
                      (value[i] >= 'a' && value[i] <= 'f')))
                    return -1;
            }

            memcpy(token, value, 64);
            token[64] = '\0';
            return 0;
        }

        p = end;

        if (*p == ';')
            p++;
    }

    return -1;
}

static int valid_file_path(const char *path)
{
    if (!path)
        return 0;

    size_t len = strlen(path);

    if (len == 0 || len > 1024)
        return 0;

    const char *component = path;

    while (*component) {
        const char *slash = strchr(component, '/');

        size_t size = slash
            ? (size_t)(slash - component)
            : strlen(component);

        if (size == 0 ||
            (size == 1 && component[0] == '.') ||
            (size == 2 && component[0] == '.' &&
                          component[1] == '.'))
            return 0;

        for (size_t i = 0; i < size; i++) {
            char c = component[i];

            if (!((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.'))
                return 0;
        }

        if (!slash)
            return 1;

        component = slash + 1;
    }

    return 0;
}

static int valid_upload_directory(const char *directory)
{
    if (!directory)
        return 0;

    size_t len = strlen(directory);

    if (len == 0 || len > 1024)
        return 0;

    size_t component_len = 0;

    for (size_t i = 0; i < len; i++) {
        char c = directory[i];

        if (c == '/') {
            if (component_len == 0)
                return 0;

            component_len = 0;
            continue;
        }

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;

        component_len++;
    }

    return component_len > 0;
}

static int valid_username(const char *username)
{
    size_t length = strlen(username);

    if (length == 0 || length > 64)
        return 0;

    for (size_t i = 0; i < length; i++) {
        char c = username[i];

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return 0;
    }

    return 1;
}

static int safe_upload_directory(const char *path)
{
    char current[PATH_CAPACITY];

    size_t len = strlen(path);

    if (len == 0 || len >= sizeof current)
        return 0;

    for (size_t i = 0; i <= len; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;

        if (i == 0)
            return 0;

        memcpy(current, path, i);
        current[i] = '\0';

        struct stat st;

        if (lstat(current, &st) != 0 ||
            !S_ISDIR(st.st_mode))
            return 0;
    }

    return 1;
}

static int validate_token(MYSQL *db, const char token[65],
                          char username[256])
{
    if (!db || !token || !username || strlen(token) != 64)
        return 0;

    username[0] = '\0';

    for (size_t i = 0; i < 64; i++) {
        char c = token[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }

    char query[256];

    int n = snprintf(query, sizeof query,
        "SELECT username FROM sessions "
        "WHERE token_hash = UNHEX(SHA2('%s', 256)) "
        "LIMIT 1",
        token);

    if (n < 0 || (size_t)n >= sizeof query)
        return -1;

    if (mysql_query(db, query) != 0) {
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(db);

    if (!result) {
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));
        return -1;
    }

    MYSQL_ROW row = mysql_fetch_row(result);

    int valid = 0;

    if (row && row[0]) {
        size_t len = strlen(row[0]);

        if (len > 0 && len < 256 && valid_username(row[0])) {
            memcpy(username, row[0], len + 1);
            valid = 1;
        } else {
            valid = -1;
        }
    }

    mysql_free_result(result);

    return valid;
}

static int verify_credentials(MYSQL *db,
                              const char *username,
                              const char *password)
{
    char query[256];

    int n = snprintf(query, sizeof query,
        "SELECT password_hash FROM passwords "
        "WHERE username = '%s' LIMIT 1",
        username);

    if (n < 0 || (size_t)n >= sizeof query)
        return -1;

    char hash[crypto_pwhash_STRBYTES] = {0};
    int found = 0;

    pthread_mutex_lock(&db_mutex);

    if (mysql_query(db, query) != 0) {
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(db);

    if (!result) {
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));
        pthread_mutex_unlock(&db_mutex);
        return -1;
    }

    MYSQL_ROW row = mysql_fetch_row(result);

    if (row && row[0]) {
        size_t length = strlen(row[0]);

        if (length < sizeof hash) {
            memcpy(hash, row[0], length + 1);
            found = 1;
        }
    }

    mysql_free_result(result);
    pthread_mutex_unlock(&db_mutex);

    if (!found)
        return 0;

    return crypto_pwhash_str_verify(
        hash, password, strlen(password)
    ) == 0;
}

static int delete_session(MYSQL *db, const char *token)
{
    if (!token || strlen(token) != 64)
        return -1;

    char query[256];

    int n = snprintf(query, sizeof query,
                     "DELETE FROM sessions "
                     "WHERE token_hash = UNHEX(SHA2('%s', 256))",
                     token);

    if (n < 0 || (size_t)n >= sizeof query)
        return -1;

    pthread_mutex_lock(&db_mutex);

    int result = mysql_query(db, query);

    if (result != 0)
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));

    pthread_mutex_unlock(&db_mutex);

    return result == 0 ? 0 : -1;
}

static int create_session(MYSQL *db,
                          const char *username,
                          char token[65])
{
    if (generate_token(token) != 0)
        return -1;

    char query[512];

    int n = snprintf(query, sizeof query,
        "INSERT INTO sessions (token_hash, username) "
        "VALUES (UNHEX(SHA2('%s', 256)), '%s')",
        token, username);

    if (n < 0 || (size_t)n >= sizeof query)
        return -1;

    pthread_mutex_lock(&db_mutex);

    int result = mysql_query(db, query);

    if (result != 0)
        fprintf(stderr, "MariaDB: %s\n", mysql_error(db));

    pthread_mutex_unlock(&db_mutex);

    return result == 0 ? 0 : -1;
}

static void respond_login(int client, const char token[65])
{
    char response[512];

    int n = snprintf(response, sizeof response,
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: __Host-mycal_session=%s; "
        "Path=/; Secure; HttpOnly; SameSite=Strict\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 17\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "\r\n"
        "Login successful\n",
        token);

    if (n < 0 || (size_t)n >= sizeof response)
        return;

    size_t sent = 0;

    while (sent < (size_t)n) {
        ssize_t count = send(client, response + sent,
                             (size_t)n - sent, 0);

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return;

        sent += (size_t)count;
    }
}

static void respond(int client, int code, const char *reason, const char *message)
{
    char response[512];
    int n = snprintf(response, sizeof response,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n%s",
        code, reason, strlen(message), message);

    if (n < 0 || (size_t)n >= sizeof response) return;

    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t count = send(client, response + sent, (size_t)n - sent, 0);
        if (count <= 0) return;
        sent += (size_t)count;
    }
}

//static void respond_logout(int client, int code,
//                           const char *reason, const char *message)
//{
//    char response[512];
//
//    int n = snprintf(response, sizeof response,
//        "HTTP/1.1 %d %s\r\n"
//        "Content-Type: text/plain; charset=utf-8\r\n"
//        "Content-Length: %zu\r\n"
//        "Connection: close\r\n"
//        "Set-Cookie: __Host-mycal_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict\r\n"
//        "X-Content-Type-Options: nosniff\r\n"
//        "\r\n"
//        "%s",
//        code, reason, strlen(message), message);
//
//    if (n < 0 || (size_t)n >= sizeof response)
//        return;
//
//    size_t sent = 0;
//
//    while (sent < (size_t)n) {
//        ssize_t count = send(client, response + sent,
//                             (size_t)n - sent, 0);
//
//        if (count <= 0)
//            return;
//
//        sent += (size_t)count;
//    }
//}

static int number(const char *text, int *value)
{
    if (!*text) return 0;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9') return 0;

    errno = 0;
    char *end;
    long n = strtol(text, &end, 10);
    if (errno || *end || n > INT_MAX) return 0;
    *value = (int)n;
    return 1;
}

static int days_in_month(int year, int month)
{
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return days[month - 1] + (month == 2 && leap);
}

/* Replace one day's CSV record; empty description removes it. */
static int update_csv(const char *username, int year, int month, int day, const char *description)
{

    char path[256], temporary[288];

    int n = snprintf(path, sizeof path,
                     "data_cal/%s/cal_%d_%d.csv",
                     username, year, month);

    if (n < 0 || (size_t)n >= sizeof path)
        return -1;

    n = snprintf(temporary, sizeof temporary,
                 "%s.tmpXXXXXX", path);

    if (n < 0 || (size_t)n >= sizeof temporary)
        return -1;

    FILE *input = fopen(path, "r");
    if (!input && errno != ENOENT) return -1;

    int fd = mkstemp(temporary);
    if (fd < 0) {
        if (input) fclose(input);
        return -1;
    }

    FILE *output = fdopen(fd, "w");
    if (!output) {
        close(fd);
        unlink(temporary);
        if (input) fclose(input);
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    int found = 0, failed = 0;

    if (input) {
        while (getline(&line, &capacity, input) != -1) {
            char *comma = strchr(line, ',');
            int old_day, matches = 0;

            if (comma) {
                *comma = '\0';
                matches = number(line, &old_day) && old_day == day;
                *comma = ',';
            }

            if (matches) {
                if (!found && *description &&
                    fprintf(output, "%d,%s\n", day, description) < 0)
                    failed = 1;
                found = 1;
            } else {
                size_t len = strlen(line);
                if (fputs(line, output) == EOF ||
                    (len && line[len - 1] != '\n' && fputc('\n', output) == EOF))
                    failed = 1;
            }
            if (failed) break;
        }
        if (ferror(input)) failed = 1;
    }

    if (!found && *description && !failed &&
        fprintf(output, "%d,%s\n", day, description) < 0)
        failed = 1;

    free(line);
    if (input && fclose(input) != 0) failed = 1;
    if (fclose(output) != 0) failed = 1;

    if (!failed && rename(temporary, path) == 0) return 0;
    unlink(temporary);
    return -1;
}

static void redirect_to_login(int client)
{
    const char response[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: /login/index.html\r\n"
        "Content-Length: 0\r\n"
        "Cache-Control: no-store\r\n"
        "Set-Cookie: __Host-mycal_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict\r\n"
        "Connection: close\r\n"
        "\r\n";

    size_t sent = 0;
    size_t length = sizeof response - 1;

    while (sent < length) {
        ssize_t n = send(client, response + sent,
                         length - sent, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return;

        sent += (size_t)n;
    }
}

static void redirect_to_month(int client, const char *username,
                              int year, int month)
{
    char response[512];

    int n = snprintf(response, sizeof response,
        "HTTP/1.1 302 Found\r\n"
        "Location: /generated/%s/mycal_%d_%d.html\r\n"
        "Content-Length: 0\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        username, year, month);

    if (n < 0 || (size_t)n >= sizeof response)
        return;

    size_t sent = 0;

    while (sent < (size_t)n) {
        ssize_t count = send(client, response + sent,
                             (size_t)n - sent, 0);

        if (count <= 0)
            return;

        sent += (size_t)count;
    }
}

static int generate_calendar(const char *username, int year, int month, int force)
{
    char path[256];

    int n = snprintf(path, sizeof path,
                     "generated/%s/mycal_%d_%d.html",
                     username, year, month);

    if (n < 0 || (size_t)n >= sizeof path)
        return -1;

    if (!force) {
        struct stat st;

        if (stat(path, &st) == 0)
            return S_ISREG(st.st_mode) ? 0 : -1;

        if (errno != ENOENT)
            return -1;
    }

    char command[256];

    n = snprintf(command, sizeof command,
                 "./calendar %s %d %d",
                 username, year, month);

    if (n < 0 || (size_t)n >= sizeof command)
        return -1;

    return system(command) == 0 ? 0 : -1;
}

static char *find_header_end(char *buffer, size_t length)
{
    if (length < 4)
        return NULL;

    for (size_t i = 0; i <= length - 4; i++) {
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0)
            return buffer + i;
    }

    return NULL;
}

static int valid_line_endings(const char *buffer, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (buffer[i] == '\r') {
            if (i + 1 >= length || buffer[i + 1] != '\n')
                return 0;

            i++;
        } else if (buffer[i] == '\n') {
            return 0;
        }
    }

    return 1;
}

static int valid_header(const char *header)
{
    const char *colon = strchr(header, ':');

    if (!colon || colon == header)
        return 0;

    for (const unsigned char *p =
             (const unsigned char *)header;
         p < (const unsigned char *)colon; p++) {

        unsigned char c = *p;

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              strchr("!#$%&'*+-.^_`|~", c)))
            return 0;
    }

    for (const unsigned char *p =
             (const unsigned char *)colon + 1;
         *p; p++) {

        if (*p == '\t')
            continue;

        if (*p < 0x20 || *p == 0x7f)
            return 0;
    }

    return 1;
}

static int authorized_uri(const char *uri, const char *username)
{
    if (!uri || !username || !valid_username(username))
        return 0;

    const char *path;

    if (strncmp(uri, "/static/", 8) == 0)
        path = uri + 8;
    else if (strncmp(uri, "/generated/", 11) == 0)
        path = uri + 11;
    else if (strncmp(uri, "/data_cal/", 10) == 0)
        path = uri + 10;
    else
        return 0;

    size_t len = strlen(username);

    if (strncmp(path, username, len) != 0)
        return 0;

    // Require the username to be a complete path component.
    if (path[len] != '/' && path[len] != '\0')
        return 0;

    return 1;
}

static int directory_is_empty(const char *path)
{
    DIR *dir = opendir(path);

    if (!dir)
        return -1;

    struct dirent *entry;
    int empty = 1;

    errno = 0;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        if (strcmp(name, ".") == 0 ||
            strcmp(name, "..") == 0 ||
            strcmp(name, "index.html") == 0)
            continue;

        empty = 0;
        break;
    }

    if (entry == NULL && errno != 0)
        empty = -1;

    closedir(dir);

    return empty;
}

static int regenerate_calendars(const char *username)
{
    char path[PATH_CAPACITY];

    int n = snprintf(path, sizeof path,
                     "generated/%s", username);

    if (n < 0 || (size_t)n >= sizeof path)
        return -1;

    DIR *dir = opendir(path);

    if (!dir)
        return -1;

    struct dirent *entry;
    int result = 0;

    while (1) {
        errno = 0;
        entry = readdir(dir);

        if (!entry) {
            if (errno != 0)
                result = -1;
            break;
        }

        int year, month;
        int end = -1;

        if (sscanf(entry->d_name,
                   "mycal_%d_%d.html%n",
                   &year, &month, &end) != 2 ||
            end < 0 ||
            entry->d_name[end] != '\0')
            continue;

        if (year < 1970 || year > 9999 ||
            month < 1 || month > 12)
            continue;

        char command[128];

        n = snprintf(command, sizeof command,
                     "./calendar %s %d %d",
                     username, year, month);

        if (n < 0 || (size_t)n >= sizeof command ||
            system(command) != 0) {
            result = -1;
            break;
        }
    }

    if (closedir(dir) != 0)
        result = -1;

    return result;
}

static void handle(int client, MYSQL *db)
{
    char request[MAX_REQUEST + 1];
    size_t used = 0;
    char *separator = NULL;

    while (!separator && used < MAX_HEADER_SIZE) {
        ssize_t n = recv(client, request + used,
                         MAX_HEADER_SIZE - used, 0);
    
        if (n < 0 && errno == EINTR)
            continue;
    
        if (n <= 0)
            return;
    
        used += (size_t)n;
        separator = find_header_end(request, used);
    }

    if (!separator) {
        respond(client, 431, "Payload Too Large", "HTTP headers too large\n");
        return;
    }

    size_t header_size = (size_t)(separator - request) + 4;

    if (memchr(request, '\0', header_size)) {
        respond(client, 400, "Bad Request",
                "NUL byte in HTTP headers\n");
        return;
    }

    if (!valid_line_endings(request, header_size)) {
        respond(client, 400, "Bad Request",
                "Malformed HTTP line endings\n");
        return;
    }

    request[used] = '\0';
    *separator = '\0';

    char *body = request + header_size;

    char *first_line_end = strstr(request, "\r\n");
    if (!first_line_end) {
        respond(client, 400, "Bad Request", "Malformed request line\n");
        return;
    }

    *first_line_end = '\0';

    int login = strcmp(request, "POST /login HTTP/1.1") == 0;
    int logout = strcmp(request, "GET /logout HTTP/1.1") == 0;
    int edit = strcmp(request, "POST /events HTTP/1.1") == 0;
    int navigate = strcmp(request, "POST /navigate HTTP/1.1") == 0;
    int current = strcmp(request, "GET /current HTTP/1.1") == 0;
    int upload = strcmp(request, "POST /upload HTTP/1.1") == 0;
    int auth_check = strcmp(request, "GET /auth/check HTTP/1.1") == 0;
    int create_directory = strcmp(request, "POST /directory/create HTTP/1.1") == 0; 
    int delete_directory = strcmp(request, "POST /directory/delete HTTP/1.1") == 0;
    int delete_file = strcmp(request, "POST /file/delete HTTP/1.1") == 0;

    if (!edit && !navigate && !current && !upload && !login && 
        !logout && !auth_check && !create_directory && !delete_directory && !delete_file) {
        respond(client, 404, "Not Found", "Unknown endpoint\n");
        return;
    }

    int protected_route = edit || navigate || current || upload || 
                          logout || auth_check || create_directory || delete_directory || delete_file;
    int requires_body = login || edit || navigate || upload;
    int csrf_protected = login || edit || navigate || upload || 
                         create_directory || delete_directory || delete_file;
    size_t content_length = 0;
    int have_length = 0;
    int custom_header = 0;
    int have_custom_header = 0;
    int have_host = 0;
    int transfer_encoding = 0;
    char *filename = NULL;
    char *upload_directory = NULL;
    char *directory = NULL;
    char *header = first_line_end + 2;
    char *cookie_header = NULL;
    char *origin = NULL;
    char *requested_uri = NULL;
    char *file_path = NULL;

    while (*header) {
        char *next = strstr(header, "\r\n");
        if (next) *next = '\0';

        if (!valid_header(header)) {
            respond(client, 400, "Bad Request",
                    "Malformed HTTP header\n");
            return;
        }

        if (strncasecmp(header, "Content-Length:", 15) == 0) {
            if (have_length) {
                respond(client, 400, "Bad Request",
                        "Duplicate Content-Length\n");
                return;
            }
        
            char *value = header + 15;
            while (*value == ' ' || *value == '\t')
                value++;
        
            if (*value < '0' || *value > '9') {
                respond(client, 400, "Bad Request",
                        "Invalid Content-Length\n");
                return;
            }
        
            errno = 0;
            char *end;
            unsigned long long size = strtoull(value, &end, 10);
        
            while (*end == ' ' || *end == '\t')
                end++;
        
            if (errno || *end || size > SIZE_MAX) {
                respond(client, 400, "Bad Request",
                        "Invalid Content-Length\n");
                return;
            }
        
            content_length = (size_t)size;
            have_length = 1;
        } else if (strncasecmp(header, "X-MyCal-Request:", 16) == 0) {

            if (have_custom_header) {
                respond(client, 400, "Bad Request",
                        "Duplicate custom header\n");
                return;
            }

            have_custom_header = 1;

            char *value = header + 16;

            while (*value == ' ' || *value == '\t')
                value++;

            custom_header = strcmp(value, "1") == 0;

        } else if (strncasecmp(header, "X-Filename:", 11) == 0) {
            if (filename) {
                respond(client, 400, "Bad Request",
                        "Duplicate X-Filename\n");
                return;
            }

            char *value = header + 11;

            while (*value == ' ' || *value == '\t')
                value++;

            size_t len = strlen(value);

            while (len > 0 &&
                   (value[len - 1] == ' ' || value[len - 1] == '\t'))
                value[--len] = '\0';

            if (len == 0 || len > 512) {
                respond(client, 400, "Bad Request",
                        "Invalid filename\n");
                return;
            }

            filename = value;
        } else if (strncasecmp(header, "Transfer-Encoding:", 18) == 0) {
            transfer_encoding = 1;
        } else if (strncasecmp(header, "Cookie:", 7) == 0) {
            if (cookie_header) {
                respond(client, 400, "Bad Request",
                        "Duplicate Cookie header\n");
                return;
            }

            cookie_header = header + 7;
        
            while (*cookie_header == ' ' || *cookie_header == '\t')
                cookie_header++;
        } else if (strncasecmp(header, "Origin:", 7) == 0) {
            if (origin) {
                respond(client, 400, "Bad Request",
                        "Duplicate Origin header\n");
                return;
            }
        
            origin = header + 7;
        
            while (*origin == ' ' || *origin == '\t')
                origin++;
        } else if (strncasecmp(header, "Host:", 5) == 0) {
            if (have_host) {
                respond(client, 400, "Bad Request",
                        "Duplicate Host header\n");
                return;
            }

            have_host = 1;

            char *value = header + 5;

            while (*value == ' ' || *value == '\t')
                value++;

            if (!*value) {
                respond(client, 400, "Bad Request",
                        "Empty Host header\n");
                return;
            }
        } else if (strncasecmp(header, "X-Requested-URI:", 16) == 0) {
            if (requested_uri) {
                respond(client, 400, "Bad Request",
                        "Duplicate X-Requested-URI\n");
                return;
            }
        
            requested_uri = header + 16;
        
            while (*requested_uri == ' ' || *requested_uri == '\t')
                requested_uri++;
        } else if (strncasecmp(header, "X-Upload-Directory:", 19) == 0) {
            if (upload_directory) {
                respond(client, 400, "Bad Request",
                        "Duplicate X-Upload-Directory\n");
                return;
            }
        
            upload_directory = header + 19;
        
            while (*upload_directory == ' ' || *upload_directory == '\t')
                upload_directory++;
        } else if (strncasecmp(header, "X-Directory:", 12) == 0) {
            if (directory) {
                respond(client, 400, "Bad Request",
                        "Duplicate X-Directory\n");
                return;
            }
        
            directory = header + 12;
        
            while (*directory == ' ' || *directory == '\t')
                directory++;
        } else if (strncasecmp(header, "X-File:", 7) == 0) {
            if (file_path) {
                respond(client, 400, "Bad Request",
                        "Duplicate X-File header\n");
                return;
            }
        
            file_path = header + 7;
        
            while (*file_path == ' ' || *file_path == '\t')
                file_path++;
        }

        if (!next) break;
        header = next + 2;
    }

    if (csrf_protected && !custom_header) {
        respond(client, 403, "Forbidden",
                "Missing X-MyCal-Request header\n");
        return;
    }

    if (csrf_protected) {
        if (!origin ||
            strcmp(origin, "https://mycal.julienlargetpiet.tech") != 0) {
            respond(client, 403, "Forbidden",
                    "Invalid request origin\n");
            return;
        }
    }

    if (transfer_encoding) {
        respond(client, 400, "Bad Request", "Transfer-Encoding unsupported\n");
        return;
    }
    if (requires_body && (!have_length || content_length == 0)) {
        respond(client, 411, "Length Required", "Missing body\n");
        return;
    }

    if (!have_host) {
        respond(client, 400, "Bad Request",
                "Missing Host header\n");
        return;
    }

    size_t received = used - header_size;
    
    if (received > content_length) {
        respond(client, 400, "Bad Request", "Extra request data\n");
        return;
    }

    if (!requires_body &&
        ((have_length && content_length != 0) || received != 0)) {
        respond(client, 400, "Bad Request",
                "Unexpected request body\n");
        return;
    }

    char username[65];
    char token[65];

    if (protected_route) {
    
        if (!cookie_header ||
            retrieve_token(cookie_header, token) != 0) {
        
            if (current)
                redirect_to_login(client);
            else
                respond(client, 401, "Unauthorized",
                        "Missing or invalid session token\n");
        
            return;
        }

        pthread_mutex_lock(&db_mutex); 
        int valid = validate_token(db, token, username);
        pthread_mutex_unlock(&db_mutex);

        if (valid == 0) {
            if (current)
                redirect_to_login(client);
            else
                respond(client, 401, "Unauthorized", "Invalid session\n");
        
            return;
        }
        
        if (valid < 0) {
            respond(client, 500, "Internal Server Error",
                    "Session validation failed\n");
            return;
        }

    }

    if (auth_check) {
        if (!authorized_uri(requested_uri, username)) {
            respond(client, 403, "Forbidden",
                    "Access denied\n");
            return;
        }
    
        respond(client, 200, "OK", "Authorized\n");
        return;
    }

    if (logout) {

        if (delete_session(db, token) != 0) {
            respond(client, 500, "Internal Server Error",
                    "Error deleting session\n");
            return;
        }

        redirect_to_login(client);        

        return;

    }

    if (current) {
        time_t now = time(NULL);
        struct tm today;
    
        if (now == (time_t)-1 ||
            localtime_r(&now, &today) == NULL) {
            respond(client, 500, "Internal Server Error",
                    "Unable to determine current date\n");
            return;
        }
    
        int year = today.tm_year + 1900;
        int month = today.tm_mon + 1;
    
        pthread_mutex_lock(&csv_mutex);
    
        int result = generate_calendar(username, year, month, 0);
    
        pthread_mutex_unlock(&csv_mutex);
    
        if (result != 0) {
            respond(client, 500, "Internal Server Error",
                    "Calendar generation failed\n");
            return;
        }
    
        redirect_to_month(client, username, year, month);

        return;
    }

    /* Binary upload route */
    if (upload) {
        if (content_length > MAX_UPLOAD_SIZE) {
            respond(client, 413, "Payload Too Large",
                    "File exceeds 25 MiB\n");
            return;
        }
    
        if (!filename) {
            respond(client, 400, "Bad Request",
                    "Missing X-Filename header\n");
            return;
        }
   
        if (!upload_directory) {
            respond(client, 400, "Bad Request",
                    "Missing X-Upload-Directory\n");
            return;
        }

        if (!(valid_upload_directory(upload_directory))) {
            return;
        }

        char upload_path[PATH_CAPACITY];
        
        int n = snprintf(upload_path, sizeof upload_path,
                         "static/%s/%s", username, upload_directory);
        
        if (n < 0 || (size_t)n >= sizeof upload_path ||
            !safe_upload_directory(upload_path))
            return;

        char command[256];

        int index_result = -1;

        pthread_mutex_lock(&upload_mutex);
   
        int result = upload_file(
            client,
            username,
            upload_directory,
            filename,
            content_length,
            (const unsigned char *)body,
            received
        );

        if (result == 0) {

            int n = snprintf(command, sizeof command,
                             "./files_index %s", username);
            
            if (n < 0 || (size_t)n >= sizeof command)
                index_result = -1;
            else
                index_result = system(command);

        }

        pthread_mutex_unlock(&upload_mutex);
    
        if (result != 0) {
            respond(client, 400, "Bad Request",
                    "Upload failed\n");
            return;
        }
    
        if (index_result != 0) {
            respond(client, 500, "Internal Server Error",
                    "File saved, but listing generation failed\n");
            return;
        }
    
        respond(client, 200, "OK", "Upload successful\n");
        return;
    }

    if (create_directory || delete_directory) {
    
        if (!directory || !valid_upload_directory(directory)) {
            respond(client, 400, "Bad Request", "Invalid directory path\n");
            return;
        }
    
        char root[PATH_CAPACITY];
    
        int n = snprintf(root, sizeof root, "static/%s/", username);
    
        if (n < 0 || (size_t)n >= sizeof root) {
            respond(client, 500, "Internal Server Error", "Invalid root path\n");
            return;
        }
    
        if (strncmp(directory, root, (size_t)n) != 0) {
            respond(client, 403, "Forbidden", "Unauthorized directory path\n");
            return;
        }
    
        char parent[PATH_CAPACITY];
    
        size_t len = strlen(directory);
    
        if (len >= sizeof parent) {
            respond(client, 400, "Bad Request", "Directory path too long\n");
            return;
        }
    
        memcpy(parent, directory, len + 1);
    
        char *slash = strrchr(parent, '/');
    
        if (!slash) {
            respond(client, 400, "Bad Request", "Invalid directory path\n");
            return;
        }
    
        *slash = '\0';
    
        int result;
    
        pthread_mutex_lock(&upload_mutex);
    
        if (create_directory) {
    
            if (!safe_upload_directory(parent)) {
                pthread_mutex_unlock(&upload_mutex);
                respond(client, 400, "Bad Request", "Unsafe parent directory\n");
                return;
            }
    
            result = mkdir(directory, 0755);
    
        } else {
    
            if (!safe_upload_directory(directory)) {
                pthread_mutex_unlock(&upload_mutex);
                respond(client, 400, "Bad Request", "Unsafe directory\n");
                return;
            }
    
            int empty = directory_is_empty(directory);
            
            if (empty != 1) {
                pthread_mutex_unlock(&upload_mutex);
            
                respond(client, empty == 0 ? 409 : 500,
                        empty == 0 ? "Conflict" : "Internal Server Error",
                        empty == 0
                            ? "Directory is not empty\n"
                            : "Unable to inspect directory\n");
                return;
            }
            
            char index_path[PATH_CAPACITY];
            
            n = snprintf(index_path, sizeof index_path,
                         "%s/index.html", directory);
            
            if (n < 0 || (size_t)n >= sizeof index_path) {
                pthread_mutex_unlock(&upload_mutex);
                respond(client, 400, "Bad Request", "Index path too long\n");
                return;
            }
            
            if (unlink(index_path) != 0 && errno != ENOENT) {
                pthread_mutex_unlock(&upload_mutex);
                respond(client, 500, "Internal Server Error",
                        "Unable to remove directory index\n");
                return;
            }
            
            result = rmdir(directory);

        }
    
        if (result != 0) {
            pthread_mutex_unlock(&upload_mutex);
    
            perror(create_directory ? "mkdir" : "rmdir");
    
            respond(client, 400, "Bad Request",
                    "Directory operation failed\n");
            return;
        }
    
        char command[128];
    
        n = snprintf(command, sizeof command,
                     "./files_index %s", username);
    
        int index_result = -1;
    
        if (n >= 0 && (size_t)n < sizeof command)
            index_result = system(command);
   
        int calendar_result = -1;
        
        if (index_result == 0)
            calendar_result = regenerate_calendars(username);
        
        pthread_mutex_unlock(&upload_mutex);
        
        if (index_result != 0 || calendar_result != 0) {
            respond(client, 500, "Internal Server Error",
                    "Directory updated, but HTML regeneration failed\n");
            return;
        }

        respond(client, 200, "OK",
                create_directory
                    ? "Directory created\n"
                    : "Directory removed\n");
    
        return;
    }

    if (delete_file) {
        if (!file_path || !valid_file_path(file_path)) {
            respond(client, 400, "Bad Request",
                    "Invalid file path\n");
            return;
        }

        char root[PATH_CAPACITY];
        
        int n = snprintf(root, sizeof root, "static/%s/", username);
        
        if (n < 0 || (size_t)n >= sizeof root) {
            respond(client, 500, "Internal Server Error", "Invalid root path\n");
            return;
        }
        
        if (strncmp(file_path, root, (size_t)n) != 0) {
            respond(client, 403, "Forbidden", "Unauthorized file path\n");
            return;
        }

        char parent[PATH_CAPACITY];

        size_t len = strlen(file_path);
        
        if (len >= sizeof parent) {
            respond(client, 400, "Bad Request", "File path too long\n");
            return;
        }
        
        memcpy(parent, file_path, len + 1);

        pthread_mutex_lock(&upload_mutex);

        char *slash = strrchr(parent, '/');
    
        if (!slash) {
            pthread_mutex_unlock(&upload_mutex);
            respond(client, 400, "Bad Request",
                    "Invalid file path\n");
            return;
        }
    
        *slash = '\0';
    
        if (!safe_upload_directory(parent)) {
            pthread_mutex_unlock(&upload_mutex);
            respond(client, 403, "Forbidden",
                    "Unsafe file directory\n");
            return;
        }
    
        struct stat st;
    
        if (lstat(file_path, &st) != 0) {
            pthread_mutex_unlock(&upload_mutex);
            respond(client, 404, "Not Found",
                    "File not found\n");
            return;
        }
    
        if (!S_ISREG(st.st_mode)) {
            pthread_mutex_unlock(&upload_mutex);
            respond(client, 400, "Bad Request",
                    "Not a regular file\n");
            return;
        }
   
        const char *basename = strrchr(file_path, '/');
        basename = basename ? basename + 1 : file_path;
        
        if (strcmp(basename, "index.html") == 0) {
            pthread_mutex_unlock(&upload_mutex);
            respond(client, 403, "Forbidden",
                    "Cannot delete generated index files\n");
            return;
        }
    
        int result = unlink(file_path);
    
        if (result == 0) {
            char command[128];
    
            n = snprintf(command, sizeof command,
                         "./files_index %s", username);
    
            if (n < 0 || (size_t)n >= sizeof command ||
                system(command) != 0)
                result = -1;
        }
    
        pthread_mutex_unlock(&upload_mutex);
    
        if (result != 0) {
            respond(client, 500, "Internal Server Error",
                    "File deletion or index generation failed\n");
            return;
        }
    
        respond(client, 200, "OK", "File deleted\n");
        return;
    }

    if (content_length > MAX_REQUEST - header_size ||
        (login && content_length > MAX_LOGIN_SIZE)) {
        respond(client, 413, "Payload Too Large",
                "Request body too large\n");
        return;
    }

    while (received < content_length) {
        ssize_t n = recv(client,
                         body + received,
                         content_length - received,
                         0);
    
        if (n < 0 && errno == EINTR)
            continue;
    
        if (n <= 0)
            return;
    
        received += (size_t)n;
    }

    if (memchr(body, '\0', content_length) ||
        memchr(body, '\r', content_length) ||
        memchr(body, '\n', content_length)) {
        respond(client, 400, "Bad Request", "Invalid event text\n");
        return;
    }
    body[content_length] = '\0';

    if (login) {
        char *colon = strchr(body, ':');
    
        if (!colon) {
            respond(client, 400, "Bad Request",
                    "Invalid credentials format\n");
            return;
        }
    
        *colon = '\0';
    
        char *username = body;
        char *password = colon + 1;
    
        if (!valid_username(username) || !*password) {
            respond(client, 400, "Bad Request",
                    "Invalid credentials format\n");
            return;
        }
    
        int valid = verify_credentials(db, username, password);
    
        if (valid < 0) {
            respond(client, 500, "Internal Server Error",
                    "Authentication error\n");
            return;
        }
    
        if (!valid) {
            respond(client, 401, "Unauthorized",
                    "Invalid username or password\n");
            return;
        }
    
        char local_token[65];
    
        if (create_session(db, username, local_token) != 0) {
            respond(client, 500, "Internal Server Error",
                    "Unable to create session\n");
            return;
        }
    
        respond_login(client, local_token);
        return;
    }

    int year, month, day;
    const char *description = NULL;

    if (navigate) {
        char *comma = strchr(body, ',');
    
        if (!comma) {
            respond(client, 400, "Bad Request", "Expected YEAR,MONTH\n");
            return;
        }
    
        *comma = '\0';
    
        if (!number(body, &year) ||
            !number(comma + 1, &month) ||
            year < 1970 || month < 1 || month > 12) {
            respond(client, 400, "Bad Request", "Invalid month\n");
            return;
        }
    
    }  else {
        char *first = strchr(body, ',');
        char *second = first ? strchr(first + 1, ',') : NULL;
        char *third = second ? strchr(second + 1, ',') : NULL;
    
        if (!third) {
            respond(client, 400, "Bad Request",
                    "Expected YEAR,MONTH,DAY,events\n");
            return;
        }
    
        *first = *second = *third = '\0';
    
        if (!number(body, &year) ||
            !number(first + 1, &month) ||
            !number(second + 1, &day) ||
            year < 1970 ||
            month < 1 || month > 12 ||
            day < 1 || day > days_in_month(year, month)) {
            respond(client, 400, "Bad Request", "Invalid date\n");
            return;
        }

        description = third + 1;

    }
    
    pthread_mutex_lock(&csv_mutex);
    
    int result = 0;
    
    if (edit)
        result = update_csv(username, year, month, day, description);
    
    if (result == 0)
        result = generate_calendar(username, year, month, edit);
    
    pthread_mutex_unlock(&csv_mutex);
    
    if (result != 0) {
        respond(client, 500, "Internal Server Error",
                "Calendar operation failed\n");
        return;
    }
    
    if (navigate) {

        char url[256];

        int n = snprintf(url, sizeof url,
                         "/generated/%s/mycal_%d_%d.html",
                         username, year, month);

        if (n < 0 || (size_t)n >= sizeof url) {
            respond(client, 500, "Internal Server Error",
                    "Generated URL too long\n");
            return;
        }

        respond(client, 200, "OK", url);

    } else {
        respond(client, 200, "OK", "Event saved\n");
    }

}

static void *client_thread(void *arg)
{
    ClientArgs *args = arg;

    int client = args->client;
    MYSQL *db = args->db;

    free(args);

    handle(client, db);
    close(client);

    return NULL;
}

int main(void)
{

    MYSQL *db = mysql_init(NULL);

    if (!db) {
        return 1;
    }

    if (!(mysql_real_connect(db,
                             "localhost",
                             "mycal",
                             "your-password",
                             "mycal",
                             0,
                             NULL,
                             0))) { 
        fprintf(stderr, "%s\n", mysql_error(db));
        mysql_close(db);
        return 1;
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium initialization failed\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) != 0) {
        perror("setsockopt");
        close(server);
        return 1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(server, 8) != 0) {
        perror("bind/listen");
        close(server);
        return 1;
    }

    printf("MyCal API listening on 127.0.0.1:%d\n", PORT);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof peer;
        int client = accept(server, (struct sockaddr *)&peer, &peer_length);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip))
            printf("Client: %s:%u\n", ip, (unsigned)ntohs(peer.sin_port));

        struct timeval timeout = {.tv_sec = TIMEOUT_SECONDS, .tv_usec = 0};
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0) {
            perror("setsockopt timeout");
            close(client);
            continue;
        }

        ClientArgs *args = malloc(sizeof *args);
        
        if (!args) {
            close(client);
            continue;
        }
        
        args->client = client;
        args->db = db;        

        pthread_t thread;
        
        if (pthread_create(&thread, NULL, client_thread, args) != 0) {
            free(args);
            close(client);
            continue;
        }
        
        pthread_detach(thread);

    }

    close(server);
    return 1;
}


