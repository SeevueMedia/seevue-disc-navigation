/*
 * The BD-J worker's operating-system sandbox and resource caps.
 *
 * BD-J runs code from the disc. bdnavd starts Java only inside a sandbox:
 *   Linux  Landlock (file access) and a seccomp filter (no sockets, no new
 *          processes, no signals or tracing outside this process), applied by
 *          bdnavd itself before it reads its first command;
 *   macOS  the server starts bdnavd under sandbox-exec; bdnavd checks that it
 *          is inside one.
 * The server asks for it with BDNAVD_SANDBOX=1 and lists the directories:
 *   BDNAVD_SANDBOX_READ   newline-separated paths readable (the runtime)
 *   BDNAVD_SANDBOX_WRITE  newline-separated paths readable and writable
 * Caps, applied on both systems when set (soft and hard):
 *   BDNAVD_RLIMIT_AS, BDNAVD_RLIMIT_FSIZE (bytes), BDNAVD_RLIMIT_NOFILE, BDNAVD_RLIMIT_CPU (seconds),
 *   BDNAVD_NICE (added to the process's priority).
 *
 * Licence: GPL-2.0-or-later.
 */
#ifndef SEEVUE_SANDBOX_H
#define SEEVUE_SANDBOX_H
#include <stddef.h>

/* Applies the caps, then the sandbox if BDNAVD_SANDBOX asks for one. Returns
   what the hello reports: "landlock+seccomp", "seatbelt", "none" (not asked)
   or "unavailable" (asked, but it could not be applied; err says why). */
const char *sandbox_apply(char *err, size_t errlen);

#endif
