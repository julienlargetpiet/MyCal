# MyCal

![logo](assets/logo.png)

## What is this ?

MyCal is a lightweight, self-hosted web application for managing calendars, events, and files.

It features a custom HTTP server written in C, CSV-based event storage, file management, and user authentication backed by MariaDB and C.

Nginx handles HTTPS, static file serving, and reverse proxying.

## Configuration

First, we'll download Nginx (if that's not already done).

On Debian based systems:

```bash

sudo apt update && sudo apt install nginx

```

On ArchLinux based systems:

```bash

sudo pacman -Syu nginx

```

Also mariadb:

On Debian based systems:

```bash

sudo apt update && sudo apt install mariadb-client mariadb-server
sudo systemctl enable --now mariadb

```

On ArchLinux based systems:

```bash

sudo pacman -Syu mariadb
sudo mariadb-install-db --user=mysql --basedir=/usr --datadir=/var/lib/mysql
sudo systemctl enable --now mariadb

```

Now, create the mariadb user:

```bash

sudo mariadb

> CREATE DATABASE mycal;
> CREATE user 'mycal'@'localhost' IDENTIFIED BY 'your-password';
> GRANT ALL PRIVILES ON mycal.* TO 'mycal'@'localhost';

```

Make sure to replace `'your-password'` by your password.

Also, in `server.c`, do the same thing for `'your-password'` in:

```c

(mysql_real_connect(db,
                    "localhost",
                    "mycal",
                    "your-password",
                    "mycal",
                    0,
                    NULL,
                    0))

```

(Line 1786)

On your VPS, generate the NGINX authentication password with `htpasswd`.

So we'll download it.

On Debian based systems:

```bash

sudo apt update && sudo apt install apache2-utils

```

On ArchLinux based systems:

```bash

sudo pacman -Syu apache

```

Then, we generate the password file:

```bash

sudo htpasswd -c -B /etc/nginx/mycal.htpasswd username

```

You'll be prompted to enter and confirm a password.

Configure a domain or subdomain with an A record pointing to your VPS's public IPv4 address (and an AAAA record if you're using IPv6).

We'll generate the SSL certifications keys with `certbot`.

Download it on Debian based systems:

```bash

sudo apt update && sudo apt install certbot

```

Download it on ArchLinux based systems:

```bash

sudo pacman -Syu certbot

```

We'll temporary stop Nginx to allow `certbot` to work.

```bash

sudo systemctl stop nginx

```

And now:

```bash

DOMAIN="mycal.example.com"

sudo certbot certonly --standalone -d "$DOMAIN" 

```

This will generate the certificates at:

```

/etc/letsencrypt/live/mycal.example.com/fullchain.pem
/etc/letsencrypt/live/mycal.example.com/privkey.pem

```

Now, modify the `USER` and `DOMAIN` variables to match your setup in `script.sh` and run it:

```bash

bash script.sh

```

Verify the NGINX synthax:

```bash

sudo nginx -t

```

And, activate it:

```bash

sudo systemctl enable --now nginx

```

Now, we'll create the `systemd` service (`/etc/systemd/system/mycal.service`), that will contain:

```

[Unit]
Description=MyCal HTTP Server
After=network.target

[Service]
Type=simple
User=your-username
Group=your-username
WorkingDirectory=/var/www/MyCal
ExecStart=/var/www/MyCal/server
Restart=on-failure
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target

```

Change `your-username` with your actual username and related group.

Make sure `systemd` sees the service:

```bash

sudo systemctl daemon-reload

```

Now, enable and start the service.

```bash

sudo systemctl enable --now mycal.service

```

Verify:

```bash

sudo systemctl status mycal.service

```








