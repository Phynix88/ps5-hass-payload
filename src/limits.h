/* All limits and fixed values in one place.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_LIMITS_H
#define PS5H_LIMITS_H

#define PS5H_DEFAULT_PORT        8181
#define PS5H_MAX_CONNS           8
#define PS5H_BACKLOG             16
#define PS5H_READ_DEADLINE_MS    3000
#define PS5H_TOTAL_DEADLINE_MS   10000
#define PS5H_MAX_HEADER          8192
#define PS5H_MAX_BODY            4096
#define PS5H_MAX_LABEL           64
#define PS5H_MAX_TOKENS          8
#define PS5H_MAX_ALLOW           16
#define PS5H_MAX_HOSTS           8
#define PS5H_CODE_TTL_S          120
#define PS5H_CODE_RESTART_S      30
#define PS5H_CODE_MAX_FAILS      5
#define PS5H_GLOBAL_MAX_FAILS    10
#define PS5H_LOCK_BASE_S         900
#define PS5H_LOCK_MAX_S          86400
#define PS5H_LAST_USED_WRITE_S   600
#define PS5H_STANDBY_INTERVAL_S  10
#define PS5H_PARAM_MAX           65536
#define PS5H_ICON_MAX            1048576
#define PS5H_STREAM_CHUNK        65536
#define PS5H_LOG_MAX             65536 /* log.txt + log.txt.1 together */
#define PS5H_IFACE_CHECK_S       30
#define PS5H_BIND_RETRY_S        10
#define PS5H_WAKE_JUMP_S         5 /* wall vs. monotonic jump = woke up */
#define PS5H_SUSPEND_GAP_S       15 /* loop pass gap = process was suspended */
#define PS5H_REBIND_MIN_MS       1000 /* at most one rebind per second */
#define PS5H_REBIND_LOG_S        60 /* at most one rebind log line per minute */
#define PS5H_MAX_APPS            512
#define PS5H_NOTIFY_MAX          200 /* bytes of a TV message */
#define PS5H_NOTIFY_BURST        5   /* messages ... */
#define PS5H_NOTIFY_WINDOW_S     30  /* ... per sliding window */
#define PS5H_MAX_PAYLOADS        32
#define PS5H_FAN_MIN_C           45  /* thresholds we may set (as ps5upload) */
#define PS5H_FAN_MAX_C           85
#define PS5H_FAN_STOCK_MIN_C     45  /* plausible factory values */
#define PS5H_FAN_STOCK_MAX_C     91  /* highest known factory value (FW 13.60) */
#define PS5H_FAN_FIGHT_RESTORES  3   /* restores ... */
#define PS5H_FAN_FIGHT_WINDOW_S  60  /* ... within this window = another tool */
#define PS5H_FAN_CHECK_S         5
#define PS5H_FAN_MAX_FAILS       5
#define PS5H_MAX_PROCS           512 /* process names read per scan */
#define PS5H_KEEP_AWAKE_S        240 /* keep-awake tick, below the 5 min limit */
#define PS5H_POWER_FAIL_OPEN_S   60  /* power state unreadable this long = "on" */
#define PS5H_PLAYTIME_POLL_S     5   /* foreground sample */
#define PS5H_PLAYTIME_SAVE_S     60  /* checkpoint of the running session */
#define PS5H_PLAYTIME_GAP_S      30  /* longer without a sample ends the session */
#define PS5H_PLAYTIME_KEEP_DAYS  400 /* older sessions are folded into totals */
#define PS5H_PLAYTIME_MAX_SESSIONS 20000
#define PS5H_PLAYTIME_MAX_TITLES 2048
#define PS5H_PLAYTIME_MAX_FILE   (4 * 1024 * 1024) /* larger = set aside */
#define PS5H_PLAYTIME_DEFAULT_DAYS 8 /* GET /playtime without ?since= */
#define PS5H_SONYTIME_REFRESH_S  3600 /* app.db is read at most this often */
#define PS5H_SONYTIME_RETRY_S    60   /* after a failed read; kept values stay */
#define PS5H_SONYTIME_MAX_COPY   (8 * 1024 * 1024) /* larger app.db = skipped */
#define PS5H_SONYTIME_MAX_TITLES 4096
#define PS5H_SOC_POWER_CACHE_S   10   /* SoC power is re-read at most this often */
#define PS5H_DRIVES_CACHE_S      60   /* external drives, same */
#define PS5H_DRIVE_MIN_MB        1024 /* smaller = a mount point without a drive */
#define PS5H_PLDMGR_PORT         8084      /* Payload Manager, on this console */
#define PS5H_PLDMGR_TIMEOUT_MS   2000      /* per phase: list, then action */
#define PS5H_PLDMGR_MAX_ANSWER   (256 * 1024)
#define PS5H_PLDMGR_MAX_TOKENS   8192      /* JSON tokens per answer */
#define PS5H_PLDMGR_MAX_ENTRIES  128       /* payloads in one list */
#define PS5H_DATA_DIR            "/data/ps5-hass"

#endif
