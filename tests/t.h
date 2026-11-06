/* Minimal test harness. Each tests/test_<x>.c is its own program.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_T_H
#define PS5H_T_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int t_failed_count, t_passed_count, t_fail;
static const char *t_current = "";

#define T_ASSERT(c)                                                        \
  do {                                                                     \
    if (!(c)) {                                                            \
      fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", __FILE__,       \
              __LINE__, t_current, #c);                                    \
      t_fail = 1;                                                          \
      return;                                                              \
    }                                                                      \
  } while (0)

#define T_EQ_INT(a, b)                                                     \
  do {                                                                     \
    long long t_a_ = (long long)(a), t_b_ = (long long)(b);                \
    if (t_a_ != t_b_) {                                                    \
      fprintf(stderr, "%s:%d: %s: %s == %s failed: %lld != %lld\n",        \
              __FILE__, __LINE__, t_current, #a, #b, t_a_, t_b_);          \
      t_fail = 1;                                                          \
      return;                                                              \
    }                                                                      \
  } while (0)

#define T_EQ_STR(a, b)                                                     \
  do {                                                                     \
    const char *t_a_ = (a), *t_b_ = (b);                                   \
    if (!t_a_ || !t_b_ || strcmp(t_a_, t_b_)) {                            \
      fprintf(stderr, "%s:%d: %s: %s == %s failed:\n  got:  %s\n  want: %s\n", \
              __FILE__, __LINE__, t_current, #a, #b, t_a_ ? t_a_ : "(null)",   \
              t_b_ ? t_b_ : "(null)");                                     \
      t_fail = 1;                                                          \
      return;                                                              \
    }                                                                      \
  } while (0)

#define T_RUN(fn)                                                          \
  do {                                                                     \
    t_current = #fn;                                                       \
    t_fail = 0;                                                            \
    fn();                                                                  \
    if (t_fail) {                                                          \
      t_failed_count++;                                                    \
    } else {                                                               \
      t_passed_count++;                                                    \
    }                                                                      \
  } while (0)

#define T_MAIN()                                                           \
  do {                                                                     \
    printf("%s: %d passed, %d failed\n", __FILE__, t_passed_count,         \
           t_failed_count);                                                \
    return t_failed_count;                                                 \
  } while (0)

/* --- helpers for tests that need files ------------------------------- */

/* Fresh empty directory below $TMPDIR (or /tmp). Static buffer. */
__attribute__((unused)) static const char *t_tmpdir(void) {
  static char path[512];
  const char *base = getenv("TMPDIR");
  snprintf(path, sizeof(path), "%s/ps5h-test-XXXXXX",
           base && *base ? base : "/tmp");
  if (!mkdtemp(path)) {
    perror("mkdtemp");
    exit(2);
  }
  return path;
}

/* mkdir -p */
__attribute__((unused)) static int t_mkdirs(const char *path) {
  char buf[1024];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) {
    return -1;
  }
  memcpy(buf, path, n + 1);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(buf, 0755) && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  return (mkdir(buf, 0755) && errno != EEXIST) ? -1 : 0;
}

/* Write a file, creating parent directories. */
__attribute__((unused)) static int t_write(const char *path, const void *data, size_t len) {
  char dir[1024];
  const char *slash = strrchr(path, '/');
  int fd;
  if (slash && (size_t)(slash - path) < sizeof(dir)) {
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    if (t_mkdirs(dir)) {
      return -1;
    }
  }
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }
  if (write(fd, data, len) != (ssize_t)len) {
    close(fd);
    return -1;
  }
  return close(fd);
}

__attribute__((unused)) static int t_write_str(const char *path, const char *s) {
  return t_write(path, s, strlen(s));
}

/* Read a whole file into a static buffer (NUL-terminated). */
__attribute__((unused)) static const char *t_read(const char *path) {
  static char buf[1 << 20];
  int fd = open(path, O_RDONLY);
  ssize_t n;
  if (fd < 0) {
    return NULL;
  }
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0) {
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

#endif
