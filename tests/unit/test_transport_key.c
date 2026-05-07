/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "ipman_key.h"

#include <assert.h>
#include <sodium.h>
#include <string.h>
#include <stdio.h>

static void test_derive_passphrase_deterministic(void) {
    const unsigned char passphrase[] = "hunter2";
    unsigned char salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0xAB, sizeof salt);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt, key1) == 0);
    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) == 0);
    printf("PASS: derive_passphrase is deterministic\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

static void test_derive_passphrase_different_salt(void) {
    const unsigned char passphrase[] = "hunter2";
    unsigned char salt1[crypto_pwhash_SALTBYTES];
    unsigned char salt2[crypto_pwhash_SALTBYTES];
    memset(salt1, 0x11, sizeof salt1);
    memset(salt2, 0x22, sizeof salt2);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt1, key1) == 0);
    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt2, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) != 0);
    printf("PASS: different salts produce different keys\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

static void test_derive_passphrase_different_passphrase(void) {
    const unsigned char pass1[] = "hunter2";
    const unsigned char pass2[] = "hunter3";
    unsigned char salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0x55, sizeof salt);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(pass1, sizeof pass1 - 1, salt, key1) == 0);
    assert(ipman_key_derive_passphrase(pass2, sizeof pass2 - 1, salt, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) != 0);
    printf("PASS: different passphrases produce different keys\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

int main(void) {
    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init failed\n");
        return 1;
    }
    test_derive_passphrase_deterministic();
    test_derive_passphrase_different_salt();
    test_derive_passphrase_different_passphrase();
    printf("All transport key tests passed.\n");
    return 0;
}
