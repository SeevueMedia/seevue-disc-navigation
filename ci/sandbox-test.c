/* Synthetic Linux boundary probe: targets are created by sandbox-test.sh. */
#include "sandbox.h"
#include <assert.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#endif
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
static void *thread(void *value) { return value; }
int main(int argc, char **argv) {
  assert(argc == 3);
  #ifdef __linux__
  if (!strcmp(argv[1], "missing")) {
    struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_landlock_create_ruleset, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { .len = 4, .filter = filter };
    assert(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0);
  }
  #endif
  setvbuf(stdout, NULL, _IONBF, 0);
  char err[160] = {0};
  const char *state = sandbox_apply(err, sizeof err);
  printf("%s %s\n", state, err);
  if (!strcmp(argv[1], "missing")) { assert(!strcmp(state, "unavailable")); return 0; }
  #ifdef __linux__
  assert(strstr(state, "+seccomp"));
  #else
  assert(!strcmp(state, "seatbelt"));
  #endif
  assert(open(argv[2], O_RDONLY) == -1 && (errno == EACCES || errno == EPERM));
  int fd = open("allowed", O_CREAT | O_RDWR, 0600); assert(fd >= 0);
  assert(write(fd, "12345678", 8) == 8);
  assert(write(fd, "x", 1) == -1 && errno == EFBIG); close(fd);
  #ifdef __linux__
  assert(malloc(1024ULL * 1024 * 1024) == NULL);
  #endif
  #ifdef __linux__
  assert(socket(AF_INET, SOCK_STREAM, 0) == -1);
  assert(socket(AF_INET6, SOCK_DGRAM, 0) == -1);
  assert(socket(AF_UNIX, SOCK_STREAM, 0) == -1);
  #else
  int sock = socket(AF_INET, SOCK_STREAM, 0); assert(sock >= 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(atoi(getenv("TEST_PORT"))), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(connect(sock, (struct sockaddr *)&addr, sizeof addr) == -1 && (errno == EPERM || errno == EACCES));
  close(sock);
  #endif
  assert(fork() == -1);
  #ifdef __linux__
  assert(vfork() == -1);
  #endif
  char *args[] = {"test-owned-child", NULL};
  assert(execv(argv[2], args) == -1);
  pthread_t tid; assert(pthread_create(&tid, NULL, thread, NULL) == 0);
  assert(pthread_join(tid, NULL) == 0);
  #ifdef __linux__
  assert(fcntl(STDIN_FILENO, F_SETOWN, getppid()) == -1);
  assert(syscall(SYS_ptrace, 0, 0, 0, 0) == -1);
  assert(syscall(SYS_unshare, 0) == -1);
  assert(syscall(SYS_io_uring_setup, 0, NULL) == -1);
  #endif
  struct rlimit limit; assert(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_max == 64);
  #ifdef __linux__
  assert(getrlimit(RLIMIT_AS, &limit) == 0 && limit.rlim_max == 536870912);
  #endif
  assert(getrlimit(RLIMIT_CPU, &limit) == 0 && limit.rlim_max == 10);
  assert(getpriority(PRIO_PROCESS, 0) >= 10);
  #ifdef __linux__
  assert(syscall(SYS_prlimit64, getppid(), RLIMIT_NOFILE, NULL, &limit) == -1);
  struct rlimit raise = {128, 128};
  assert(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &raise, NULL) == -1);
  #endif
  puts("sandbox boundaries passed");
  return 0;
}
