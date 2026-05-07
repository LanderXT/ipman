/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "passphrase.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int ipman_read_passphrase(const char *prompt, char *out, size_t cap,
                          size_t *out_len) {
    /* Test / scripting escape hatch: skip tty interaction entirely. */
    const char *env = getenv("IPMAN_PASSPHRASE");
    if (env != NULL) {
        size_t len = strlen(env);
        if (len == 0) {
            fprintf(stderr, "ipman: IPMAN_PASSPHRASE is set but empty\n");
            return -1;
        }
        if (len >= cap) {
            fprintf(stderr, "ipman: IPMAN_PASSPHRASE too long (max %zu)\n",
                    cap - 1);
            return -1;
        }
        memcpy(out, env, len);
        out[len] = '\0';
        *out_len = len;
        return 0;
    }

    int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) {
        fprintf(stderr, "ipman: cannot open /dev/tty: %s\n", strerror(errno));
        return -1;
    }

    /* Suppress echo for the duration of the read. */
    struct termios old, noecho;
    if (tcgetattr(tty_fd, &old) != 0) {
        fprintf(stderr, "ipman: tcgetattr failed: %s\n", strerror(errno));
        close(tty_fd);
        return -1;
    }
    noecho = old;
    noecho.c_lflag &= (tcflag_t)~(ECHO | ECHOE | ECHOK | ECHONL);
    if (tcsetattr(tty_fd, TCSAFLUSH, &noecho) != 0) {
        fprintf(stderr, "ipman: tcsetattr failed: %s\n", strerror(errno));
        close(tty_fd);
        return -1;
    }

    /* Write prompt directly to tty (not stdout, which may be redirected). */
    if (prompt != NULL) {
        (void)write(tty_fd, prompt, strlen(prompt));
    }

    size_t pos = 0;
    int rc = 0;
    while (pos < cap - 1) {
        char c;
        ssize_t n = read(tty_fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "\nipman: read passphrase failed: %s\n",
                    strerror(errno));
            rc = -1;
            break;
        }
        if (n == 0 || c == '\n' || c == '\r') break;
        out[pos++] = c;
    }
    out[pos] = '\0';

    /* Restore echo before any early-return path. */
    (void)tcsetattr(tty_fd, TCSAFLUSH, &old);
    (void)write(tty_fd, "\n", 1);
    close(tty_fd);

    if (rc != 0) return -1;

    if (pos == 0) {
        fprintf(stderr, "ipman: passphrase must not be empty\n");
        return -1;
    }

    *out_len = pos;
    return 0;
}
