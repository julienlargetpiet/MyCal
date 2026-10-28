#!/bin/bash

USR="username"
DOMAIN="domain-name"

set -e

sed "s|YOUR_DOMAIN|$DOMAIN|g" mycal.conf

mv mycal.conf /etc/nginx/sites-available/.

cd /var/www/MyCal

gcc -std=c11 -O3 -Wall -Wextra -Wpedantic -pthread \
    $(mariadb_config --cflags) \
    server.c upload.c \
    -o server \
    $(mariadb_config --libs) -lsodium

gcc -std=c11 -O3 -Wall -Wextra -Wpedantic calendar.c -o calendar
gcc -std=c11 -O3 -Wall -Wextra -Wpedantic files_index.c -o files_index

chown -R "$USR":"$USR" /var/www/MyCal

chmod 755 /var/www/MyCal

chmod 700 server calendar files_index

find data_cal -type d -exec chmod 700 {} +
find data_cal -type f -name '*.csv' -exec chmod 600 {} +

find generated -type d -exec chmod 755 {} +
find generated -type f -name '*.html' -exec chmod 644 {} +

find static -type d -exec chmod 755 {} +
find static -type f -exec chmod 644 {} +

chmod 644 mycal.html calendar.css calendar.js files.css files_index.js

chmod 644 /var/www/MyCal/login/index.html
chmod 755 /var/www/MyCal/login

chmod 755 /var/www/MyCal/assets
chmod 644 /var/www/MyCal/assets/favicon.png

for dir in static/*/; do
    [ -d "$dir" ] || continue

    username="${dir#static/}"
    username="${username%/}"

    runuser -u "$USR" -- ./files_index "$username"
done



