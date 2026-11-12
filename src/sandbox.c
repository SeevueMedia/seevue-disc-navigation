/* The BD-J worker's sandbox and caps: see sandbox.h. Licence: GPL-2.0-or-later. */
#include "sandbox.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int env_u64(const char *name, uint64_t *out) {
  const char *v = getenv(name);
  if (!v || !*v || *v == '-') return 0;
  char *end = NULL;
  errno = 0;
  unsigned long long n = strtoull(v, &end, 10);
  if (errno || !end || *end) return 0;
  *out = (uint64_t)n;
  return 1;
}

static int cap(int resource, const char *name) {
  uint64_t n;
  if (!getenv(name)) return 0;
  if (!env_u64(name, &n) || (rlim_t)n == RLIM_INFINITY) { errno = EINVAL; return -1; }
  struct rlimit r = {(rlim_t)n, (rlim_t)n};
  return setrlimit(resource, &r);
}

static int limits_apply(char *err, size_t errlen) {
  struct rlimit none = {0, 0};
  if (setrlimit(RLIMIT_CORE, &none) != 0 ||
      cap(RLIMIT_AS, "BDNAVD_RLIMIT_AS") != 0 ||
      cap(RLIMIT_FSIZE, "BDNAVD_RLIMIT_FSIZE") != 0 ||
      cap(RLIMIT_NOFILE, "BDNAVD_RLIMIT_NOFILE") != 0 ||
      cap(RLIMIT_CPU, "BDNAVD_RLIMIT_CPU") != 0) {
    snprintf(err, errlen, "resource limits: %s", strerror(errno));
    return -1;
  }
  signal(SIGXFSZ, SIG_IGN);
  uint64_t nice_by;
  if (env_u64("BDNAVD_NICE", &nice_by) && nice_by <= 19) {
    errno = 0;
    if (nice((int)nice_by) == -1 && errno) {
      snprintf(err, errlen, "nice: %s", strerror(errno));
      return -1;
    }
  }
  return 0;
}

static int asked(void) {
  const char *v = getenv("BDNAVD_SANDBOX");
  return v && !strcmp(v, "1");
}

#if defined(__APPLE__)

/* libsystem_sandbox: 1 when the process runs under a sandbox profile. */
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

const char *sandbox_apply(char *err, size_t errlen) {
  if (limits_apply(err, errlen) != 0) return "unavailable";
  if (!asked()) return "none";
  if (sandbox_check(getpid(), NULL, 0) == 1) return "seatbelt";
  snprintf(err, errlen, "not started under sandbox-exec");
  return "unavailable";
}

#elif defined(__linux__)

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

/* Rights newer than the build's kernel headers. */
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

/* The ruleset attribute with every field up to ABI 6; the kernel reads the
   size it is given, so older kernels get the fields they know. */
struct ruleset_attr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;
  uint64_t scoped;
};

#define FS_FILE (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE)
#define FS_READ (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR)
#define FS_WRITE                                                                                         \
  (FS_READ | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
   LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG)

static uint64_t fs_handled(int abi) {
  uint64_t all = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                 LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                 LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                 LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                 LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi >= 2) all |= LANDLOCK_ACCESS_FS_REFER;
  if (abi >= 3) all |= LANDLOCK_ACCESS_FS_TRUNCATE;
  if (abi >= 5) all |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
  return all;
}

/* Adds one path; a path that does not exist is skipped. Files get only the
   file rights the directory rights imply. */
static int allow_path(int ruleset, const char *path, uint64_t access, uint64_t handled, char *err, size_t errlen) {
  int fd = open(path, O_PATH | O_CLOEXEC);
  if (fd < 0) return 0;
  struct stat st;
  if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) access &= FS_FILE | LANDLOCK_ACCESS_FS_TRUNCATE;
  struct landlock_path_beneath_attr rule = {.allowed_access = access & handled, .parent_fd = fd};
  int rc = (int)syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
  int saved = errno;
  close(fd);
  if (rc != 0) {
    snprintf(err, errlen, "landlock rule: %s", strerror(saved));
    return -1;
  }
  return 0;
}

static int allow_list(int ruleset, const char *list, uint64_t access, uint64_t handled, char *err, size_t errlen) {
  if (!list) return 0;
  char path[4096];
  while (*list) {
    const char *end = strchr(list, '\n');
    size_t n = end ? (size_t)(end - list) : strlen(list);
    if (n > 0 && n < sizeof path) {
      memcpy(path, list, n);
      path[n] = 0;
      if (path[0] == '/' && allow_path(ruleset, path, access, handled, err, errlen) < 0) return -1;
    }
    if (!end) break;
    list = end + 1;
  }
  return 0;
}

/* What every process needs to read on Linux, beyond the server's lists: the
   dynamic linker's cache, the X11 client libraries' configuration, this
   process's own /proc entry and the device nodes the JVM opens. Never
   another process's /proc entry. */
static const char *const SYSTEM_READ[] = {"/etc/ld.so.cache", "/etc/localtime", "/etc/nsswitch.conf",
                                          "/etc/passwd", "/etc/fonts", "/usr/share/fonts", "/usr/share/zoneinfo",
                                          "/proc/self", "/proc/meminfo", "/proc/cpuinfo", "/proc/stat",
                                          "/sys/devices/system/cpu", "/sys/fs/cgroup", "/dev/random", "/dev/urandom",
                                          NULL};
static const char *const SYSTEM_WRITE[] = {"/dev/null", "/dev/zero", NULL};

static int landlock_apply(char *err, size_t errlen, int *abi_out) {
  int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) {
    snprintf(err, errlen, "landlock: %s", strerror(errno));
    return -1;
  }
  *abi_out = abi;
  struct ruleset_attr attr = {.handled_access_fs = fs_handled(abi)};
  size_t size = sizeof attr.handled_access_fs;
  if (abi >= 4) {
    /* No TCP bind or connect at all (no rules allow any port). */
    attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    size = offsetof(struct ruleset_attr, scoped);
  }
  if (abi >= 6) {
    /* No abstract Unix sockets or signals outside this process's domain. */
    attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    size = sizeof attr;
  }
  int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0);
  if (ruleset < 0) {
    snprintf(err, errlen, "landlock ruleset: %s", strerror(errno));
    return -1;
  }
  uint64_t handled = attr.handled_access_fs;
  uint64_t write = FS_WRITE | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
  int rc = 0;
  for (int i = 0; SYSTEM_READ[i] && rc == 0; i++) rc = allow_path(ruleset, SYSTEM_READ[i], FS_READ, handled, err, errlen);
  for (int i = 0; SYSTEM_WRITE[i] && rc == 0; i++)
    rc = allow_path(ruleset, SYSTEM_WRITE[i], LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE, handled, err,
                    errlen);
  if (rc == 0) rc = allow_list(ruleset, getenv("BDNAVD_SANDBOX_READ"), FS_READ, handled, err, errlen);
  if (rc == 0) rc = allow_list(ruleset, getenv("BDNAVD_SANDBOX_WRITE"), write, handled, err, errlen);
  if (rc == 0 && syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
    snprintf(err, errlen, "landlock restrict: %s", strerror(errno));
    rc = -1;
  }
  close(ruleset);
  return rc;
}

/* ---------- seccomp ---------- */

#if defined(__x86_64__)
#define ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH AUDIT_ARCH_AARCH64
#else
#error "unsupported architecture for the seccomp filter"
#endif

#define FILTER_MAX 512
struct filter {
  struct sock_filter f[FILTER_MAX];
  unsigned n;
  int overflow;
};
static void emit(struct filter *p, struct sock_filter ins) {
  if (p->n >= FILTER_MAX) {
    p->overflow = 1;
    return;
  }
  p->f[p->n++] = ins;
}
#define LD(off) emit(p, (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (off)))
#define RET(v) emit(p, (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, (v)))
#define JEQ(k, t, f) emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (k), (t), (f)))
#define JSET(k, t, f) emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, (k), (t), (f)))
#define JGE(k, t, f) emit(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, (k), (t), (f)))
#define NR offsetof(struct seccomp_data, nr)
#define ARG_LO(i) offsetof(struct seccomp_data, args[i])
#define ARG_HI(i) (offsetof(struct seccomp_data, args[i]) + 4)
#define DENY(e) (SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA))

/* nr -> errno: refused outright. */
static void deny(struct filter *p, long nr, int e) {
  JEQ((uint32_t)nr, 0, 1);
  RET(DENY(e));
}

/* nr allowed only when argument 0 is this process (kill, tgkill). */
static void self_only(struct filter *p, long nr, uint32_t pid) {
  JEQ((uint32_t)nr, 0, 5);
  LD(ARG_HI(0));
  JEQ(0, 0, 2);
  LD(ARG_LO(0));
  JEQ(pid, 1, 0);
  RET(DENY(EPERM));
  LD(NR);
}

/* Restrict descriptor controls: F_SETOWN/F_SETSIG and terminal injection
   must not turn an inherited pipe into a way to signal the supervisor. */
static void commands_only(struct filter *p, long nr, const uint32_t *commands, unsigned n) {
  JEQ((uint32_t)nr, 0, 5 + 2 * n);
  LD(ARG_HI(1)); JEQ(0, 1, 0); RET(DENY(EPERM));
  LD(ARG_LO(1));
  for (unsigned i = 0; i < n; i++) {
    JEQ(commands[i], 0, 1); RET(SECCOMP_RET_ALLOW);
  }
  RET(DENY(EPERM));
  LD(NR);
}

static int seccomp_apply(char *err, size_t errlen) {
  struct filter prog = {.n = 0}, *p = &prog;
  uint32_t pid = (uint32_t)getpid();
  LD(offsetof(struct seccomp_data, arch));
  JEQ(ARCH, 1, 0);
  RET(SECCOMP_RET_KILL_PROCESS);
  LD(NR);
#if defined(__x86_64__)
  /* No x32 system calls. */
  JGE(0x40000000, 0, 1);
  RET(SECCOMP_RET_KILL_PROCESS);
#endif
  /* No network: no sockets of any family. Pipes remain available. */
  deny(p, SYS_socket, EACCES);
  /* No new programs and no new processes; threads only. */
  deny(p, SYS_execve, EPERM);
  deny(p, SYS_execveat, EPERM);
#ifdef SYS_fork
  deny(p, SYS_fork, EPERM);
#endif
#ifdef SYS_vfork
  deny(p, SYS_vfork, EPERM);
#endif
  /* clone3 hides its flags behind a pointer: glibc falls back to clone. */
#ifdef SYS_clone3
  deny(p, SYS_clone3, ENOSYS);
#endif
  JEQ((uint32_t)SYS_clone, 0, 7);
  LD(ARG_LO(0));
  JSET(CLONE_THREAD, 1, 0);
  RET(DENY(EPERM));
  LD(ARG_LO(0));
  JSET(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWIPC |
       CLONE_NEWUTS | CLONE_NEWCGROUP | CLONE_PTRACE | CLONE_UNTRACED, 0, 1);
  RET(DENY(EPERM));
  LD(NR);
  /* Signals only within this worker. */
  self_only(p, SYS_kill, pid);
  self_only(p, SYS_tgkill, pid);
  /* libc queries and lowers our own limits with pid 0. */
  self_only(p, SYS_prlimit64, 0);
  /* Query only: disc code cannot raise a hard limit, even with privilege. */
  JEQ(SYS_prlimit64, 0, 6);
  LD(ARG_HI(2)); JEQ(0, 0, 3);
  LD(ARG_LO(2)); JEQ(0, 1, 0);
  RET(DENY(EPERM));
  LD(NR);
  static const uint32_t fcntl_commands[] = {F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD,
      F_GETFL, F_SETFL, F_GETLK, F_SETLK, F_SETLKW};
  static const uint32_t ioctl_commands[] = {FIONREAD, FIONBIO, TCGETS, TIOCGWINSZ};
  commands_only(p, SYS_fcntl, fcntl_commands, sizeof fcntl_commands / sizeof fcntl_commands[0]);
  commands_only(p, SYS_ioctl, ioctl_commands, sizeof ioctl_commands / sizeof ioctl_commands[0]);
  /* Default deny. File opens are further restricted by Landlock. No socket,
     exec, fork, tracing, namespace, mount, or privileged kernel interfaces. */
  static const long allowed[] = {
      SYS_read, SYS_write, SYS_readv, SYS_writev, SYS_pread64, SYS_pwrite64,
      SYS_close, SYS_lseek, SYS_openat, SYS_fstat, SYS_newfstatat,
      SYS_getdents64, SYS_readlinkat, SYS_faccessat,
      SYS_dup, SYS_dup3, SYS_pipe2, SYS_fsync, SYS_fdatasync, SYS_ftruncate,
      SYS_mkdirat, SYS_unlinkat, SYS_renameat, SYS_fchmod,
      SYS_mmap, SYS_mprotect, SYS_munmap, SYS_mremap, SYS_madvise, SYS_brk,
      SYS_rt_sigaction, SYS_rt_sigprocmask, SYS_rt_sigreturn, SYS_sigaltstack,
      SYS_futex, SYS_set_robust_list, SYS_get_robust_list, SYS_set_tid_address,
      SYS_getpid, SYS_getppid, SYS_gettid, SYS_getuid, SYS_geteuid, SYS_getgid,
      SYS_getegid, SYS_getresuid, SYS_getresgid, SYS_getgroups,
      SYS_clock_gettime, SYS_clock_getres, SYS_clock_nanosleep, SYS_nanosleep,
      SYS_gettimeofday, SYS_times, SYS_getrusage, SYS_uname, SYS_sysinfo,
      SYS_sched_yield, SYS_sched_getaffinity, SYS_sched_getparam,
      SYS_sched_getscheduler, SYS_sched_get_priority_min, SYS_sched_get_priority_max,
      SYS_getpriority, SYS_getrandom, SYS_getcwd, SYS_chdir,
      SYS_statfs, SYS_fstatfs, SYS_getrlimit,
      SYS_ppoll, SYS_pselect6, SYS_epoll_create1, SYS_epoll_ctl, SYS_epoll_pwait,
      SYS_eventfd2, SYS_restart_syscall, SYS_exit, SYS_exit_group,
#ifdef SYS_rseq
      SYS_rseq,
#endif
#ifdef SYS_statx
      SYS_statx,
#endif
#ifdef SYS_faccessat2
      SYS_faccessat2,
#endif
#ifdef SYS_membarrier
      SYS_membarrier,
#endif
#if defined(__x86_64__)
      SYS_open, SYS_stat, SYS_lstat, SYS_access, SYS_readlink, SYS_dup2,
      SYS_pipe, SYS_poll, SYS_select, SYS_epoll_wait, SYS_epoll_create,
      SYS_mkdir, SYS_unlink, SYS_rename, SYS_rmdir,
      SYS_arch_prctl, SYS_time,
#endif
  };
  for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
    JEQ((uint32_t)allowed[i], 0, 1);
    RET(SECCOMP_RET_ALLOW);
  }
  /* self_only and the clone check return here only for valid calls. */
  JEQ(SYS_kill, 0, 1); RET(SECCOMP_RET_ALLOW);
  JEQ(SYS_tgkill, 0, 1); RET(SECCOMP_RET_ALLOW);
  JEQ(SYS_clone, 0, 1); RET(SECCOMP_RET_ALLOW);
  JEQ(SYS_prlimit64, 0, 1); RET(SECCOMP_RET_ALLOW);
  RET(DENY(EPERM));
  if (prog.overflow) {
    snprintf(err, errlen, "seccomp: filter too long");
    return -1;
  }
  struct sock_fprog fprog = {.len = (unsigned short)prog.n, .filter = prog.f};
  if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
    snprintf(err, errlen, "seccomp: %s", strerror(errno));
    return -1;
  }
  return 0;
}

const char *sandbox_apply(char *err, size_t errlen) {
  if (limits_apply(err, errlen) != 0) return "unavailable";
  if (!asked()) return "none";
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    snprintf(err, errlen, "no_new_privs: %s", strerror(errno));
    return "unavailable";
  }
  int abi = 0;
  if (landlock_apply(err, errlen, &abi) != 0) return "unavailable";
  if (seccomp_apply(err, errlen) != 0) return "unavailable";
  static char desc[48];
  snprintf(desc, sizeof desc, "landlock-%d+seccomp", abi);
  return desc;
}

#else
const char *sandbox_apply(char *err, size_t errlen) {
  if (limits_apply(err, errlen) != 0) return "unavailable";
  if (!asked()) return "none";
  snprintf(err, errlen, "no sandbox on this system");
  return "unavailable";
}
#endif
