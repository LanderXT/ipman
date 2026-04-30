#ifndef IPMAN_HOME_H
#define IPMAN_HOME_H

#include <stddef.h>

/*
 * Resolve and prepare the ipman workspace directory.
 *
 * Precedence: $IPMAN_HOME if set and non-empty, else ./.ipman in the
 * current working directory.
 *
 * ipman_home_resolve fills `out` with the path (NUL-terminated). The default
 * is intentionally relative so generated manifests reflect the workspace
 * convention agents should use.
 *
 * ipman_home_require verifies that the directory already exists and that its
 * mode has no bits outside 0700 set (world/group access is refused — this
 * directory will hold the user's entire plan history). Returns 0 on success.
 *
 * ipman_home_ensure performs the same validation, creating the directory
 * (mode 0700) if it does not exist.
 */

int ipman_home_resolve(char *out, size_t outlen);
int ipman_home_require(const char *path);
int ipman_home_ensure(const char *path);

#endif
