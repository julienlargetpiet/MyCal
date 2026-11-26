#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stddef.h>
#include <dirent.h>
#include <fcntl.h>

static const int year_months_days[12] =
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static const int leap_year_months_days[12] =
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static const int cumu_year_months_days[12] =
    {31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

static const int cumu_leap_year_months_days[12] =
    {31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};

static int first_weekday_month(int year, int month, int is_leap)
{
    long long years = (long long)year - 1970;
    long long y = (long long)year - 1;
    long long leaps = y / 4 - y / 100 + y / 400 - 477;
    long long days = 365 * years + leaps;

    if (month > 1) {
        if (is_leap)
            days += cumu_leap_year_months_days[month - 2];
        else
            days += cumu_year_months_days[month - 2];
    }

    return (int)((days + 3) % 7 + 1);
}

static void html_char(FILE *out, char c)
{
    switch (c) {
        case '&': fputs("&amp;", out); break;
        case '<': fputs("&lt;", out); break;
        case '>': fputs("&gt;", out); break;
        case '"': fputs("&quot;", out); break;
        case '\'': fputs("&#39;", out); break;
        default: fputc(c, out); break;
    }
}

static void html_text(FILE *out, const char *text)
{
    for (const char *p = text; *p; p++)
        html_char(out, *p);
}

static void print_upload_directories(FILE *out,
                                     const char *root,
                                     const char *relative,
                                     int depth)
{
    if (depth > 16)
        return;

    char path[4096];

    int n = relative[0]
        ? snprintf(path, sizeof path, "%s/%s", root, relative)
        : snprintf(path, sizeof path, "%s", root);

    if (n < 0 || (size_t)n >= sizeof path)
        return;

    DIR *dir = opendir(path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;

        if (name[0] == '.')
            continue;

        struct stat st;

        if (fstatat(dirfd(dir), name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode))
            continue;

        char child[4096];

        n = relative[0]
            ? snprintf(child, sizeof child, "%s/%s", relative, name)
            : snprintf(child, sizeof child, "%s", name);

        if (n < 0 || (size_t)n >= sizeof child)
            continue;

        fputs("<option value='", out);
        html_text(out, child);
        fputs("'>", out);
        html_text(out, child);
        fputs("</option>\n", out);

        print_upload_directories(out, root, child, depth + 1);
    }

    closedir(dir);
}

/* Find a sequence within a bounded string. */
static const char *find_sequence(const char *start,
                                 const char *end,
                                 const char *sequence,
                                 size_t length)
{
    for (const char *p = start; end - p >= (ptrdiff_t)length; p++) {
        if (memcmp(p, sequence, length) == 0)
            return p;
    }

    return NULL;
}

static int safe_url(const char *url, size_t length)
{

    int absolute =
        (length >= 7 && memcmp(url, "http://", 7) == 0) ||
        (length >= 8 && memcmp(url, "https://", 8) == 0);

    int relative =
        length >= 2 && url[0] == '/' && url[1] != '/';

    if (!absolute && !relative)
        return 0;

    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)url[i];

        if (c <= 32 || c == 127 || c == '<' || c == '>')
            return 0;
    }

    return 1;
}

/* Render inline Markdown in a bounded text range. */
static void markdown_inline(FILE *out, const char *s,
                            size_t length, int depth)
{
    const char *end = s + length;

    while (s < end) {

        /* Images: ![alt](url) */
        if (end - s >= 2 && s[0] == '!' && s[1] == '[') {
            const char *close = find_sequence(s + 2, end, "](", 2);

            if (close) {
                const char *finish =
                    find_sequence(close + 2, end, ")", 1);

                if (finish &&
                    safe_url(close + 2, (size_t)(finish - close - 2))) {

                    fputs("<img src=\"", out);

                    for (const char *p = close + 2; p < finish; p++)
                        html_char(out, *p);

                    fputs("\" alt=\"", out);

                    for (const char *p = s + 2; p < close; p++)
                        html_char(out, *p);

                    fputs("\">", out);

                    s = finish + 1;
                    continue;
                }
            }
        }

        /* Videos: ?[alt](url) */
        if (end - s >= 2 && s[0] == '?' && s[1] == '[') {
            const char *close = find_sequence(s + 2, end, "](", 2);

            if (close) {
                const char *finish =
                    find_sequence(close + 2, end, ")", 1);

                if (finish &&
                    safe_url(close + 2, (size_t)(finish - close - 2))) {

                    fputs("<video src=\"", out);

                    for (const char *p = close + 2; p < finish; p++)
                        html_char(out, *p);

                    fputs("\" controls playsinline preload=\"metadata\"></video>", out);

                    s = finish + 1;
                    continue;
                }
            }
        }

        /* Links: [label](url) */
        if (*s == '[') {
            const char *close = find_sequence(s + 1, end, "](", 2);

            if (close) {
                const char *finish =
                    find_sequence(close + 2, end, ")", 1);

                if (finish &&
                    safe_url(close + 2, (size_t)(finish - close - 2))) {

                    fputs("<a href=\"", out);

                    for (const char *p = close + 2; p < finish; p++)
                        html_char(out, *p);

                    fputs("\">", out);

                    markdown_inline(out, s + 1,
                                    (size_t)(close - s - 1),
                                    depth + 1);

                    fputs("</a>", out);

                    s = finish + 1;
                    continue;
                }
            }
        }

        /* Bold: **text** */
        if (depth < 8 && end - s >= 2 &&
            s[0] == '*' && s[1] == '*') {

            const char *close =
                find_sequence(s + 2, end, "**", 2);

            if (close) {
                fputs("<strong>", out);

                markdown_inline(out, s + 2,
                                (size_t)(close - s - 2),
                                depth + 1);

                fputs("</strong>", out);

                s = close + 2;
                continue;
            }
        }

        /* Italic: *text* */
        if (depth < 8 && end - s >= 2 && s[0] == '_' && s[1] == '_') {
            const char *close =
                find_sequence(s + 2, end, "__", 1);

            if (close) {
                fputs("<em>", out);

                markdown_inline(out, s + 2,
                                (size_t)(close - s - 2),
                                depth + 1);

                fputs("</em>", out);

                s = close + 2;
                continue;
            }
        }

        /* Ordinary text: escape HTML characters. */
        html_char(out, *s);
        s++;
    }
}

/* Render the entire description, with '+' as line separator. */
static void render_markdown(FILE *out, const char *text)
{
    const char *line = text;

    for (;;) {
        const char *end = strchr(line, '+');

        if (!end)
            end = line + strlen(line);

        const char *content = line;
        int level = 0;

        /* Heading: # Title, ## Title, etc. */
        while (content < end && *content == '#' && level < 6) {
            content++;
            level++;
        }

        if (level > 0 && content < end && *content == ' ') {
            content++;

            fprintf(out, "<h%d>", level);
            markdown_inline(out, content,
                            (size_t)(end - content), 0);
            fprintf(out, "</h%d>\n", level);
        } else if (end == line) {
            fputs("<br>\n", out);
        } else {
            fputs("<div>", out);
            markdown_inline(out, line,
                            (size_t)(end - line), 0);
            fputs("</div>\n", out);
        }

        if (!*end)
            break;

        line = end + 1;
    }
}

static void print_day(FILE *out, int day, char *events[32])
{
    fputs("<td>\n"
          "  <div class=\"day-container\">\n"
          "    <span class=\"day-number\">", out);

    fprintf(out, "%d", day);

    fputs("</span>\n"
          "    <div class=\"day-events\" data-raw-description=\"", out);
    
    const char *description = events[day] ? events[day] : "";
    
    for (const char *p = description; *p; p++) {
        if (*p == '+')
            fputs("&#10;", out);
        else
            html_char(out, *p);
    }
    
    fputs("\">", out);
    
    if (*description)
        render_markdown(out, description);
    
    fputs("</div>\n", out);

    if (events[day] != NULL && events[day][0] != '\0') {
        fputs(
            "<button type='button' class='event-details-button'>"
            "View details ↗"
            "</button>\n",
            out
        );
    }

    fputs(
        "<button type='button' class='event-edit-button'>Edit</button>\n",
        out
    );

    fputs("  </div>\n"
          "</td>\n", out);
}

static void print_empty(FILE *out)
{
    fputs("<td></td>\n", out);
}

static int parse_number(const char *s, int *result)
{
    if (*s == '\0')
        return 0;

    for (const char *p = s; *p; p++) { // '\0' == 0
        if (*p < '0' || *p > '9')
            return 0;
    }

    errno = 0;
    char *end;
    long n = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || n > INT_MAX)
        return 0;

    *result = (int)n;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s USERNAME YEAR MONTH\n", argv[0]);
        return 1;
    }

    const char *username = argv[1];

    int year, month;

    if (!parse_number(argv[2], &year) ||
        !parse_number(argv[3], &month) ||
        month < 1 || month > 12) {
        fputs("Invalid year or month\n", stderr);
        return 1;
    }

    if (year < 1970) {
        fputs("Year must be >= 1970\n", stderr);
        return 1;
    }

    int is_leap = 0;
    if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
        is_leap = 1;

    int num_day_begin = first_weekday_month(year, month, is_leap);
    int days_tot;

    if (is_leap)
        days_tot = leap_year_months_days[month - 1];
    else
        days_tot = year_months_days[month - 1];

    int days_still_to_print = days_tot - (8 - num_day_begin);

    int dow[7] = {8, 9, 10, 11, 12, 13, 14};
    char *events[32] = {0};

    char csv_path[256];
    int n = snprintf(csv_path, sizeof csv_path,
                     "data_cal/%s/cal_%d_%d.csv",
                     username, year, month);
    
    if (n < 0 || (size_t)n >= sizeof csv_path)
        return 1;

    char *buffer = NULL;

    FILE *csv = fopen(csv_path, "rb");

    if (csv != NULL) {

      if (fseek(csv, 0, SEEK_END) != 0) {
          perror("fseek");
          goto csv_fail;
      }

      long size = ftell(csv);

      if (size < 0) {
          perror("ftell");
          goto csv_fail;
      }

      if (fseek(csv, 0, SEEK_SET) != 0) {
          perror("fseek");
          goto csv_fail;
      }

      buffer = malloc((size_t)size + 1);

      if (buffer == NULL) {
          perror("malloc");
          goto csv_fail;
      }

      size_t bytes = fread(buffer, 1, (size_t)size, csv);

      if (bytes != (size_t)size) {
          perror("fread");
          goto csv_fail;
      }

      buffer[bytes] = '\0';

      fclose(csv);
      csv = NULL;

      // Parse the buffer directly
      char *line = buffer;

      while (*line != '\0') {
          char *next = strchr(line, '\n');

          if (next != NULL)
              *next = '\0';

          char *comma = strchr(line, ',');

          if (comma != NULL) {
              *comma = '\0';

              int day;

              if (parse_number(line, &day) &&
                  day >= 1 && day <= days_tot) {

                  char *description = comma + 1;

                  events[day] = description;
              }
          }

          if (next == NULL)
              break;

          line = next + 1;
      }

    } else if (errno != ENOENT) {
        perror(csv_path);
        goto fail;
    }

    char destination[256];
    char temporary[288];
    
    int n2 = snprintf(destination, sizeof destination,
                      "generated/%s/mycal_%d_%d.html",
                      username, year, month);
    
    if (n2 < 0 || (size_t)n2 >= sizeof destination)
        goto fail;
    
    n2 = snprintf(temporary, sizeof temporary,
                  "%s.tmpXXXXXX", destination);
    
    if (n2 < 0 || (size_t)n2 >= sizeof temporary)
        goto fail;

    int fd = mkstemp(temporary);
    if (fd == -1) {
        perror("mkstemp");
        goto fail;
    }
   
    if (fchmod(fd, 0644) != 0) {
        perror("fchmod");
        close(fd);
        unlink(temporary);
        goto fail;
    }

    FILE *out = fdopen(fd, "w");
    if (out == NULL) {
        perror("fdopen");
        close(fd);
        unlink(temporary);
        goto fail;
    }

    fputs("<!DOCTYPE html>\n"
          "<html lang='en'>\n"
          "<head>\n"
          "    <meta charset='UTF-8'>\n"
          "    <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
          "    <link rel='stylesheet' href='/calendar.css'>\n"
          "    <script src='/calendar.js' defer></script>\n"
          "    <link rel='icon' type='image/png' href='/assets/favicon.png'>"
          "</head>\n"
          "<body>\n"
          "<button id='theme-toggle' type='button' aria-label='Toggle dark mode'>\n"
          "    <span class='theme-icon'></span>\n"
          "</button>\n", out);

    fputs(
        "<div class='calendar-navigation'>\n"
        "  <button type='button' id='previous-month'>← Previous</button>\n"
        "  <button type='button' id='next-month'>Next →</button>\n"
        "</div>\n",
        out
    );

    fprintf(out, "<table id='calendar' data-year='%d' data-month='%d'>\n",
            year, month);

    fputs("<tr>\n"
          "<th>Monday</th>\n"
          "<th>Tuesday</th>\n"
          "<th>Wednesday</th>\n"
          "<th>Thursday</th>\n"
          "<th>Friday</th>\n"
          "<th>Saturday</th>\n"
          "<th>Sunday</th>\n"
          "</tr>\n", out);

    fputs("<tr>\n", out);

    for (int i = 1; i < num_day_begin; i++)
        print_empty(out);

    for (int day = 1; day <= 8 - num_day_begin; day++)
        print_day(out, day, events);

    fputs("</tr>\n", out);

    for (int i = 0; i < 7; i++)
        dow[i] = 9 - num_day_begin + i;

    int rows = days_still_to_print / 7;
    int rmd = days_still_to_print % 7;

    for (int cnt = 0; cnt < rows; cnt++) {
        fputs("<tr>\n", out);

        print_day(out, dow[0], events);
        print_day(out, dow[1], events);
        print_day(out, dow[2], events);
        print_day(out, dow[3], events);
        print_day(out, dow[4], events);
        print_day(out, dow[5], events);
        print_day(out, dow[6], events);

        fputs("</tr>\n", out);

        dow[0] += 7;
        dow[1] += 7;
        dow[2] += 7;
        dow[3] += 7;
        dow[4] += 7;
        dow[5] += 7;
        dow[6] += 7;
    }

    if (rmd > 0) {
        fputs("<tr>\n", out);

        for (int i = 0; i < rmd; i++)
            print_day(out, dow[i], events);

        for (int i = rmd; i < 7; i++)
            print_empty(out);

        fputs("</tr>\n", out);
    }


    fputs("</table>\n\n", out);
    
    fprintf(out,
        "<div class='calendar-actions'>\n"
        "  <a href='/static/%s/' class='files-button'>See files ↗</a>\n"
        "  <a href='/logout' class='files-button'>Logout</a>\n"
        "</div>\n",
        username);

    fputs(
        "<div class='upload-controls'>\n"
        "  <input type='file' id='file-input'>\n"
        "  <label for='upload-directory'>Destination directory</label>\n"
        "  <select id='upload-directory'>\n",
        out
    );
    
    char static_root[256];
    
    n = snprintf(static_root, sizeof static_root,
                     "static/%s", username);
    
    if (n < 0 || (size_t)n >= sizeof static_root) {
        fclose(out);
        unlink(temporary);
        goto fail;
    }

    print_upload_directories(out, static_root, "", 0);
    
    fputs(
        "  </select>\n"
        "  <button type='button' id='upload-button'>Upload file</button>\n"
        "  <p id='upload-status' role='status'></p>\n"
        "</div>\n"
        "<dialog id='event-dialog' class='event-dialog'>\n"
        "    <div class='event-dialog-header'>\n"
        "        <h2 class='event-dialog-title'></h2>\n"
        "        <button class='event-dialog-close' type='button' "
                 "aria-label='Close dialog'>&times;</button>\n"
        "    </div>\n"
        "    <div class='event-dialog-description'></div>\n"
        "</dialog>\n"
        "\n",
        out
    );

    fputs(
        "<dialog id='event-edit-dialog' class='event-dialog event-editor'>\n"
        "  <form class='event-edit-form'>\n"
        "    <h2 class='event-dialog-title'>Edit events</h2>\n"
        "    <label for='event-input'>Events (Markdown supported)</label>\n"
        "    <textarea id='event-input' rows='6'></textarea>\n"
        "    <p class='event-edit-status' role='status'></p>\n"
        "    <div class='event-edit-actions'>\n"
        "      <button type='button' class='event-cancel'>Cancel</button>\n"
        "      <button type='submit' class='event-save'>Save</button>\n"
        "    </div>\n"
        "  </form>\n"
        "</dialog>\n"
        "</body>"
        "</html>",
        out
    );

    int write_failed = ferror(out);
    
    if (fclose(out) != 0 || write_failed) {
        perror("fclose");
        unlink(temporary);
        goto fail;
    }
    
    if (rename(temporary, destination) != 0) {
        perror("rename");
        unlink(temporary);
        goto fail;
    }
    
    free(buffer);

    return 0;

csv_fail:
   
    if (fclose(csv) != 0) {
        csv = NULL;
        perror("fclose");
        goto fail;
    }

csv = NULL;

fail:
    
    free(buffer);

    return 1;
}




