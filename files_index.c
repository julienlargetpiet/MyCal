#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "static"
#define MAX_DEPTH 16

static void html_char(FILE *out, unsigned char c)
{
    switch (c) {
        case '&': fputs("&amp;", out); break;
        case '<': fputs("&lt;", out); break;
        case '>': fputs("&gt;", out); break;
        case '"': fputs("&quot;", out); break;
        case '\'': fputs("&#39;", out); break;
        default: fputc(c, out);
    }
}

static void html_text(FILE *out, const char *s)
{
    for (; *s; s++)
        html_char(out, (unsigned char)*s);
}

/* Encode one filename as a relative URL component. */
static void url_component(FILE *out, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            fputc(c, out);
        } else {
            fputc('%', out);
            fputc(hex[c >> 4], out);
            fputc(hex[c & 15], out);
        }
    }
}

static int generate_directory(const char *path, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;

    DIR *dir = opendir(path);
    if (!dir) {
        perror(path);
        return -1;
    }

    char temporary[4096];
    char destination[4096];

    int n1 = snprintf(temporary, sizeof temporary,
                      "%s/.index.tmpXXXXXX", path);

    int n2 = snprintf(destination, sizeof destination,
                      "%s/index.html", path);

    if (n1 < 0 || (size_t)n1 >= sizeof temporary ||
        n2 < 0 || (size_t)n2 >= sizeof destination) {
        closedir(dir);
        return -1;
    }

    int fd = mkstemp(temporary);
    if (fd == -1) {
        perror("mkstemp");
        closedir(dir);
        return -1;
    }

    if (fchmod(fd, 0644) != 0) {
        perror("fchmod");
        close(fd);
        unlink(temporary);
        closedir(dir);
        return -1;
    }

    FILE *out = fdopen(fd, "w");
    if (!out) {
        perror("fdopen");
        close(fd);
        unlink(temporary);
        closedir(dir);
        return -1;
    }

    fputs(
        "<!DOCTYPE html>\n"
        "<html lang='en'>\n"
        "<head>\n"
        "<meta charset='UTF-8'>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<title>MyCal Files</title>\n"
        "<link rel='stylesheet' href='/files.css'>\n"
        "<link rel='icon' type='image/png' href='/assets/favicon.png'>"
        "<script>\n"
        "(function () {\n"
        "    const theme = localStorage.getItem('mycal-theme');\n"
        "    if (theme === 'dark')\n"
        "        document.documentElement.dataset.theme = 'dark';\n"
        "})();\n"
        "</script>\n"
        "<script src='/files_index.js' defer></script>\n"
        "</head>\n"
        "<body>\n"
        "<main class='files-page'>\n"
        "<p class='eyebrow'>MYCAL / FILE MANAGER</p>\n"
        "<h1>My files</h1>\n"
        "<nav class='files-nav'>\n"
        "<a href='/'>← Calendar</a>\n",
        out
    );

    if (depth > 0)
        fputs("<a href='../'>↑ Parent folder</a>\n", out);

    fputs(
        "</nav>\n"
    
        "<section class='files-actions'>\n"
    
        "<form id='create-directory-form'>\n"
        "<label for='directory-name'>New folder</label>\n"
        "<input id='directory-name' name='directory-name' "
        "type='text' placeholder='Folder name' "
        "pattern='[A-Za-z0-9_-]+' maxlength='255' required>\n"
        "<button type='submit'>Create folder</button>\n"
        "</form>\n"
    
        "<button type='button' id='delete-selected' disabled>"
        "Delete selected"
        "</button>\n"
    
        "<p id='files-message' role='status' aria-live='polite'></p>\n"
        "</section>\n"
    
        "<div class='files-list'>\n",
        out
    );

    struct dirent *entry;
    int failed = 0;

    while (1) {
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0) {
                perror("readdir");
                failed = 1;
            }
            break;
        }

        const char *name = entry->d_name;

        if (name[0] == '.')
            continue;

        if (strcmp(name, "index.html") == 0)
            continue;

        struct stat st;

        if (fstatat(dirfd(dir), name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            perror("fstatat");
            failed = 1;
            break;
        }

        int directory = S_ISDIR(st.st_mode);

        /* Ignore symbolic links and special files. */
        if (!directory && !S_ISREG(st.st_mode))
            continue;

        fputs("<div class='file-row'>\n", out);
        
        fputs("<label class='file-select'>", out);
        
        fputs("<input type='radio' name='selected-entry' value='", out);
        html_text(out, name);
        
        fputs("' data-kind='", out);
        fputs(directory ? "directory" : "file", out);
        
        fputs("' aria-label='Select ", out);
        html_text(out, name);
        fputs("'>", out);
        
        fputs("</label>\n", out);
        
        fputs("<a class='file-item' href='", out);
        
        url_component(out, name);
        
        if (directory)
            fputc('/', out);
        
        fputs("'><span class='file-icon'>", out);
        fputs(directory ? "📁" : "📄", out);
        fputs("</span><span class='file-name'>", out);
        
        html_text(out, name);
        
        if (directory)
            fputc('/', out);
        
        fputs("</span><span class='file-meta'>", out);
        
        if (directory)
            fputs("Folder", out);
        else
            fprintf(out, "%lld bytes", (long long)st.st_size);
        
        fputs("</span></a>\n", out);
        
        fputs("</div>\n", out);

        if (directory) {
            char child[4096];

            int n = snprintf(child, sizeof child,
                             "%s/%s", path, name);

            if (n < 0 || (size_t)n >= sizeof child) {
                failed = 1;
                break;
            }

            if (generate_directory(child, depth + 1) != 0) {
                failed = 1;
                break;
            }
        }
    }

    fputs(
        "</div>\n"
        "</main>\n"
        "</body>\n"
        "</html>\n",
        out
    );

    if (ferror(out))
        failed = 1;

    if (closedir(dir) != 0) {
        perror("closedir");
        failed = 1;
    }

    if (fclose(out) != 0) {
        perror("fclose");
        failed = 1;
    }

    if (failed) {
        unlink(temporary);
        return -1;
    }

    if (rename(temporary, destination) != 0) {
        perror("rename");
        unlink(temporary);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <username>\n", argv[0]);
        return 1;
    }

    const char *username = argv[1];

    char root[4096];

    int n = snprintf(root, sizeof root,
                     "static/%s", username);

    if (n < 0 || (size_t)n >= sizeof root) {
        fputs("Path too long\n", stderr);
        return 1;
    }

    if (generate_directory(root, 0) != 0) {
        fputs("Unable to generate file listings\n", stderr);
        return 1;
    }

    puts("File listings generated successfully");
    return 0;
}



