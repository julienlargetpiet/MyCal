#include <stdio.h>
#include <string.h>
#include <sodium.h>

int main(void)
{
    if (sodium_init() < 0)
        return 1;

    char password[256];
    char hash[crypto_pwhash_STRBYTES];

    printf("Password: ");

    if (!fgets(password, sizeof password, stdin))
        return 1;

    password[strcspn(password, "\n")] = '\0';

    if (crypto_pwhash_str(
            hash,
            password,
            strlen(password),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        fprintf(stderr, "Password hashing failed\n");
        return 1;
    }

    printf("%s\n", hash);
    sodium_memzero(password, sizeof password);

    return 0;
}
