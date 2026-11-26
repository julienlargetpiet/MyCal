# MyCal

<p align="center">
<img src="assets/logo.png" alt="MyCal Logo" width="150">
</p>

## What is MyCal?

MyCal is a lightweight, self-hosted web application for managing calendars, events, and files.

It features a custom HTTP server written in C, CSV-based event storage, file management, and user authentication backed by MariaDB.

Nginx handles HTTPS, static file serving, and reverse proxying.

## Installation and Configuration

### Installing Nginx

First, install Nginx if it is not already installed.

On Debian-based distributions:

```bash
sudo apt update && sudo apt install nginx
```

On Arch Linux-based distributions:

```bash
sudo pacman -Syu nginx
```

### Installing MariaDB

On Debian-based distributions:

```bash
sudo apt update && sudo apt install mariadb-client mariadb-server
sudo systemctl enable --now mariadb
```

On Arch Linux-based distributions:

```bash
sudo pacman -Syu mariadb
sudo mariadb-install-db --user=mysql --basedir=/usr --datadir=/var/lib/mysql
sudo systemctl enable --now mariadb
```

The `mariadb-install-db` command is only required when initializing a new MariaDB installation.

### Configuring the database

Open the MariaDB shell:

```bash
sudo mariadb
```

Create the database and its dedicated user:

```sql
CREATE DATABASE mycal;
CREATE USER 'mycal'@'localhost' IDENTIFIED BY 'your-password';
GRANT ALL PRIVILEGES ON mycal.* TO 'mycal'@'localhost';
EXIT;
```

Make sure to replace `your-password` with a strong database password.

Initialize the database schema using the provided SQL script:

```bash
mariadb -u mycal -p mycal < script.sql
```

### Configuring MyCal

The repository is expected to be located at:

```
/var/www/MyCal
```

In `server.c`, replace `your-password` with the database password you previously configured:

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

This configuration can be found around line 1786.

**Note:** Since the database password is currently hardcoded, avoid committing or distributing the modified source code or compiled binary containing your credentials.

### Configuring HTTP Basic Authentication

MyCal uses Nginx HTTP Basic Authentication as an additional authentication layer.

Install `htpasswd` to generate the corresponding password file.

On Debian-based distributions:

```bash
sudo apt update && sudo apt install apache2-utils
```

On Arch Linux-based distributions:

```bash
sudo pacman -Syu apache
```

Generate the password file:

```bash
sudo htpasswd -c -B /etc/nginx/mycal.htpasswd username
```

You will be prompted to enter and confirm a password.

The `-B` option uses bcrypt for password hashing, while `-c` creates a new password file.

When adding additional users to an existing password file, omit the `-c` option to avoid overwriting it.

These credentials are independent of MyCal's own user authentication system.

### Configuring DNS

Configure a domain or subdomain with an `A` record pointing to your VPS's public IPv4 address.

If your server also supports IPv6, you can configure an `AAAA` record pointing to its public IPv6 address. Make sure Nginx is also configured to listen on IPv6.

For the remainder of this documentation, we will use:

```
mycal.example.com
```

Replace it with your actual domain name.

### Generating TLS certificates

We use Certbot to obtain TLS certificates from Let's Encrypt.

On Debian-based distributions:

```bash
sudo apt update && sudo apt install certbot
```

On Arch Linux-based distributions:

```bash
sudo pacman -Syu certbot
```

Temporarily stop Nginx to allow Certbot to bind to port 80:

```bash
sudo systemctl stop nginx
```

Generate the certificates:

```bash
DOMAIN="mycal.example.com"

sudo certbot certonly --standalone -d "$DOMAIN"
```

Make sure port 80 is accessible from the Internet and that your DNS records have been correctly configured.

The certificates will be generated at:

```
/etc/letsencrypt/live/mycal.example.com/fullchain.pem
/etc/letsencrypt/live/mycal.example.com/privkey.pem
```

### Preparing MyCal

From the root of the repository, run the provided setup script:

```bash
cd /var/www/MyCal
bash script.sh
```

### Configuring Nginx

On Debian-based distributions, create the following configuration file:

```
/etc/nginx/sites-available/mycal.conf
```

On Arch Linux-based distributions, you can use:

```
/etc/nginx/conf.d/mycal.conf
```

Make sure the corresponding configuration directory is included in your main `nginx.conf` file.

Replace every occurrence of `mycal.example.com` with your actual domain name.

```nginx

# HTTP -> HTTPS
server {
    listen 80;
    server_name mycal.example.com;

    return 301 https://mycal.example.com$request_uri;
}

# MyCal HTTPS server
server {
    listen 443 ssl;
    server_name mycal.example.com;

    ssl_certificate /etc/letsencrypt/live/mycal.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/mycal.example.com/privkey.pem;

    root /var/www/MyCal;

    proxy_http_version 1.1;

    # Keep Basic Auth until the new authorization is fully tested.
    auth_basic "MyCal";
    auth_basic_user_file /etc/nginx/mycal.htpasswd;

    add_header Cache-Control "no-store, private" always;
    add_header X-Content-Type-Options "nosniff" always;

    open_file_cache off;

    # CSS
    location = /calendar.css {
        try_files /calendar.css =404;
    }

    # JavaScript
    location = /calendar.js {
        try_files /calendar.js =404;
    }

    location = /files.css {
        try_files /files.css =404;
    }

    # Calendar editing API
    location = /events {
        client_max_body_size 16k;

        proxy_pass http://127.0.0.1:18081;

        proxy_connect_timeout 3s;
        proxy_read_timeout 15s;
        proxy_send_timeout 10s;

        proxy_request_buffering on;
        proxy_set_header Connection "close";
    }

    location = /navigate {
        client_max_body_size 1k;

        proxy_pass http://127.0.0.1:18081;

        proxy_connect_timeout 3s;
        proxy_read_timeout 15s;
        proxy_send_timeout 10s;

        proxy_request_buffering on;
        proxy_set_header Connection "close";
    }

    location = / {
        proxy_pass http://127.0.0.1:18081/current;
        proxy_set_header Connection "close";

        proxy_connect_timeout 3s;
        proxy_read_timeout 15s;
    }

    # Private generated calendar pages
    location ^~ /generated/ {
        auth_request /auth/check;

        try_files $uri =404;

        add_header Cache-Control "no-store, private" always;
        add_header X-Content-Type-Options "nosniff" always;
    }

    # Private uploaded files
    location ^~ /static/ {
        auth_request /auth/check;

        disable_symlinks on from=$document_root;

        autoindex on;
        autoindex_localtime on;
        autoindex_exact_size off;

        try_files $uri $uri/ =404;

        add_header Cache-Control "no-store, private" always;
        add_header X-Content-Type-Options "nosniff" always;
    }

    location = /static {
        return 301 /static/;
    }

    # Private calendar CSV files
    location ^~ /data_cal/ {
        return 404;
    }

    # Internal authorization endpoint
    location = /auth/check {
        internal;

        auth_basic off;

        proxy_pass http://127.0.0.1:18081;

        proxy_pass_request_body off;
        proxy_set_header Content-Length "";

        proxy_set_header Host $host;
        proxy_set_header X-Requested-URI $request_uri;
    }

    # Public favicon
    location = /assets/favicon.png {
        auth_basic off;
        auth_request off;
        try_files /assets/favicon.png =404;
    }

    location = /assets/logo.jpg {
        auth_basic off;
        auth_request off;
        try_files /assets/logo.jpg =404;
    }

    location = /assets/cursors/cursor.png {
        auth_basic off;
        auth_request off;
        try_files /assets/cursors/cursor.png =404;
    }
    location = /assets/cursors/cursor-hand.png {
        auth_basic off;
        auth_request off;
        try_files /assets/cursors/cursor-hand.png =404;
    }


    location = /assets/logo.png {
        auth_basic off;
        auth_request off;
        try_files /assets/logo.png =404;
    }

    # Public login page
    location ^~ /login/ {
        auth_basic off;
        auth_request off;

        try_files $uri $uri/ =404;
    }

    location = /logout {
        proxy_pass http://127.0.0.1:18081;
    }

    # Login API
    location = /login {
        auth_basic off;
        auth_request off;

        proxy_pass http://127.0.0.1:18081;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    # Upload API
    location = /upload {
        client_max_body_size 25m;

        proxy_pass http://127.0.0.1:18081;
        proxy_set_header Connection "close";

        proxy_request_buffering on;
        proxy_read_timeout 120s;
        proxy_send_timeout 120s;
    }

    # Deny everything else
    location / {
        return 404;
    }

    location = /directory/create {
        proxy_pass http://127.0.0.1:18081;
    }
    
    location = /directory/delete {
        proxy_pass http://127.0.0.1:18081;
    }
    
    location = /file/delete {
        proxy_pass http://127.0.0.1:18081;
    }

    location = /files_index.js {
        alias /var/www/MyCal/files_index.js;
        default_type application/javascript;
    }

}


```

On Debian-based distributions, enable the configuration by creating a symbolic link:

```bash
sudo ln -s /etc/nginx/sites-available/mycal.conf \
    /etc/nginx/sites-enabled/mycal.conf
```

Verify the Nginx configuration:

```bash
sudo nginx -t
```

Enable and start Nginx:

```bash
sudo systemctl enable --now nginx
```

If Nginx is already running, reload its configuration:

```bash
sudo systemctl reload nginx
```

**Note:** Since the certificates were obtained using Certbot's standalone mode, certificate renewal requires port 80 to be available. Make sure to configure an appropriate renewal mechanism.

### Creating the systemd service

To run MyCal as a persistent system service, create the following file:

```
/etc/systemd/system/mycal.service
```

With the following contents:

```ini
[Unit]
Description=MyCal HTTP Server
After=network.target mariadb.service
Requires=mariadb.service

[Service]
Type=simple
User=your-username
Group=your-group
WorkingDirectory=/var/www/MyCal
ExecStart=/var/www/MyCal/server
Restart=on-failure
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

Replace `your-username` and `your-group` with the actual user and group under which the server should run.

Make sure this user has the necessary read and write permissions on MyCal's files and data directories.

Reload the systemd configuration:

```bash
sudo systemctl daemon-reload
```

Enable and start MyCal:

```bash
sudo systemctl enable --now mycal.service
```

Verify that the service is running:

```bash
sudo systemctl status mycal.service
```

You can also inspect the server logs using:

```bash
sudo journalctl -u mycal.service -f
```

MyCal should now be accessible at:

```
https://mycal.example.com
```

## Adding Users

MyCal stores user credentials in MariaDB and uses Argon2id for password hashing through libsodium.

### Installing libsodium

On Debian-based distributions:

```bash
sudo apt update && sudo apt install libsodium-dev
```

On Arch Linux-based distributions:

```bash
sudo pacman -Syu libsodium
```

### Compiling the password-hashing utility

The repository provides a standalone C program, `hash_password.c`, for generating password hashes.

Compile it using:

```bash
gcc -Wall -Wextra hash_password.c -o hash_password -lsodium
```

### Generating a password hash

Run the compiled program:

```bash
./hash_password
```

Enter the desired password when prompted.

The program will output an Argon2id hash in the following format:

```
$argon2id$v=19$m=65536,t=2,p=1$<salt>$<hash>
```

The salt is randomly generated, meaning that hashing the same password multiple times will produce different outputs.

### Registering the user

Edit `add_user.sql` with your desired username and the complete password hash generated in the previous step:

```sql
INSERT INTO passwords (username, password_hash)
VALUES ('username', 'your-argon2id-hash');
```

Make sure to use the entire generated hash, not the original plaintext password.

Execute the SQL script:

```bash
mariadb -u mycal -p mycal < add_user.sql
```

Finally, initialize the user's environment using the provided shell script:

```bash
bash create_user.sh USERNAME
```

Replace `USERNAME` with the same username used in the SQL query.

The user can now authenticate through MyCal's login page.



