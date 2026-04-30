#include "ipman_home.h"

#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int ipman_home_resolve(char *out, size_t outlen) {
    const char *override = getenv("IPMAN_HOME");
    if (override != NULL && override[0] != '\0') {
        int n = snprintf(out, outlen, "%s", override);
        if (n < 0 || (size_t)n >= outlen) {
            ipman_log_error("ipman_home path too long", "source=IPMAN_HOME");
            return -1;
        }
        return 0;
    }

    int n = snprintf(out, outlen, ".ipman");
    if (n < 0 || (size_t)n >= outlen) {
        ipman_log_error("ipman_home path too long", "source=default");
        return -1;
    }
    return 0;
}

static int ipman_home_validate_stat(const char *path, const struct stat *st) {
    if (!S_ISDIR(st->st_mode)) {
        ipman_log_error("ipman_home is not a directory", "path=%s", path);
        return -1;
    }
    /* Reject any bits outside 0700 — plan history is sensitive. */
    mode_t extra = st->st_mode & 0077;
    if (extra != 0) {
        ipman_log_error("ipman_home has loose permissions",
                       "path=%s mode=%04o",
                       path, (unsigned)(st->st_mode & 0777));
        return -1;
    }
    if (st->st_uid != geteuid()) {
        ipman_log_error("ipman_home not owned by current user",
                       "path=%s", path);
        return -1;
    }
    return 0;
}

int ipman_home_require(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        ipman_log_error("ipman_home missing", "path=%s errno=%d", path, errno);
        return -1;
    }
    return ipman_home_validate_stat(path, &st);
}

int ipman_home_ensure(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return ipman_home_validate_stat(path, &st);

    if (errno != ENOENT) {
        ipman_log_error("cannot stat ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    if (mkdir(path, 0700) != 0) {
        ipman_log_error("cannot create ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    if (chmod(path, 0700) != 0) {
        ipman_log_error("cannot chmod ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    ipman_log_info("created ipman_home", "path=%s mode=0700", path);
    return 0;
}
