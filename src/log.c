/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "limits.h"
#include "platform.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char last_error[256];
static bool have_error;

static void write_line(const char *line) {
  char path[700], old[710];
  struct stat st;
  size_t n = strlen(line);
  int fd;
  if (config_data_dir(path, sizeof(path))) {
    return;
  }
  snprintf(old, sizeof(old), "%s/log.txt.1", path);
  strncat(path, "/log.txt", sizeof(path) - strlen(path) - 1);
  if (!lstat(path, &st) && (size_t)st.st_size + n > PS5H_LOG_MAX / 2) {
    rename(path, old);
  }
  fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return;
  }
  if (write(fd, line, n) < 0) {
    /* nothing sensible left to do */
  }
  close(fd);
}

static void vlog(bool is_error, const char *fmt, va_list ap) {
  char msg[400], line[460], ts[21];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  config_iso_time(plat_wall_s(), ts);
  snprintf(line, sizeof(line), "[%s] %s%s\n", ts, is_error ? "ERROR " : "",
           msg);
  pthread_mutex_lock(&lock);
  if (is_error) {
    snprintf(last_error, sizeof(last_error), "%s %s", ts, msg);
    have_error = true;
  }
  write_line(line);
  pthread_mutex_unlock(&lock);
}

void log_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(false, fmt, ap);
  va_end(ap);
}

void log_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(true, fmt, ap);
  va_end(ap);
}

const char *log_last_error(void) { return have_error ? last_error : NULL; }
