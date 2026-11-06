/* mDNS announcement of _ps5-hass._tcp with microdns, in its own thread
 * (mdns_serve blocks). Pattern tested on FW 13.60.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <microdns/microdns.h>

#include "limits.h"
#include "log.h"
#include "platform.h"

#ifndef PS5H_VERSION
#define PS5H_VERSION "dev"
#endif

#define SERVICE "_ps5-hass._tcp.local"

static pthread_t thread;
static atomic_bool started, stop_flag, running, reannounce, paired_flag;
static uint16_t port;
static char host[64], instance[96], service[] = SERVICE;
static struct rr_data_txt txt[4];

static void build_txt(const char *device_id) {
  memset(txt, 0, sizeof(txt));
  snprintf(txt[0].txt, sizeof(txt[0].txt), "id=%s", device_id);
  snprintf(txt[1].txt, sizeof(txt[1].txt), "version=%s", PS5H_VERSION);
  snprintf(txt[2].txt, sizeof(txt[2].txt), "api=1");
  for (int i = 0; i < 3; i++) {
    txt[i].next = &txt[i + 1];
  }
}

/* PTR, TXT, SRV, A for one IPv4 address. */
static void send_answers(struct mdns_ctx *ctx, const struct in_addr *ip,
                         bool goodbye) {
  struct mdns_hdr hdr;
  struct rr_entry ans[4];
  memset(&hdr, 0, sizeof(hdr));
  memset(ans, 0, sizeof(ans));
  snprintf(txt[3].txt, sizeof(txt[3].txt), "paired=%d",
           atomic_load(&paired_flag) ? 1 : 0);
  hdr.flags |= FLAG_QR | FLAG_AA;
  hdr.num_ans_rr = 4;
  for (int i = 0; i < 4; i++) {
    ans[i].rr_class = RR_IN;
    ans[i].ttl = goodbye ? 0 : 120;
    ans[i].msbit = 1;
    ans[i].next = i < 3 ? &ans[i + 1] : NULL;
  }
  ans[0].type = RR_PTR;
  ans[0].name = service;
  ans[0].data.PTR.domain = instance;
  ans[1].type = RR_TXT;
  ans[1].name = instance;
  ans[1].data.TXT = &txt[0];
  ans[2].type = RR_SRV;
  ans[2].name = instance;
  ans[2].data.SRV.port = port;
  ans[2].data.SRV.target = host;
  ans[3].type = RR_A;
  ans[3].name = host;
  memcpy(&ans[3].data.A.addr, ip, sizeof(ans[3].data.A.addr));
  mdns_entries_send(ctx, &hdr, ans);
}

static void on_question(void *cookie, const struct sockaddr *ip,
                        const char *svc, enum mdns_announce_type type) {
  if (svc && strcmp(svc, SERVICE)) {
    return;
  }
  if (!ip || ip->sa_family != AF_INET) {
    return;
  }
  send_answers(cookie, &((const struct sockaddr_in *)ip)->sin_addr,
               type == MDNS_ANNOUNCE_GOODBYE);
}

static struct mdns_ctx *current;

/* Called by mdns_serve between polls: handles re-announcements. */
static bool should_stop(void *cookie) {
  (void)cookie;
  if (atomic_exchange(&reannounce, false) && current) {
    uint32_t addrs[8];
    int n = plat_ipv4_list(addrs, 8);
    for (int i = 0; i < n; i++) {
      struct in_addr a;
      a.s_addr = addrs[i];
      send_answers(current, &a, false);
    }
  }
  return atomic_load(&stop_flag);
}

static void *run(void *arg) {
  (void)arg;
  while (!atomic_load(&stop_flag)) {
    char err[128] = "";
    int rc;
    if ((rc = mdns_init(&current, MDNS_ADDR_IPV4, MDNS_PORT)) < 0) {
      mdns_strerror(rc, err, sizeof(err));
      log_error("mdns: init failed: %s", err);
      current = NULL;
    } else {
      mdns_announce(current, RR_PTR, on_question, current);
      atomic_store(&running, true);
      atomic_store(&reannounce, true); /* announce at once */
      rc = mdns_serve(current, should_stop, NULL);
      atomic_store(&running, false);
      if (rc < 0) {
        mdns_strerror(rc, err, sizeof(err));
        log_error("mdns: stopped: %s", err);
      }
      mdns_destroy(current);
      current = NULL;
    }
    for (int i = 0; i < PS5H_BIND_RETRY_S && !atomic_load(&stop_flag); i++) {
      sleep(1);
    }
  }
  return NULL;
}

int mdns_start(const char *device_id, uint16_t p, bool paired) {
  if (atomic_load(&started)) {
    return 0;
  }
  port = p;
  snprintf(host, sizeof(host), "ps5-hass-%.6s.local", device_id);
  snprintf(instance, sizeof(instance), "PS5 %.6s." SERVICE, device_id);
  build_txt(device_id);
  atomic_store(&paired_flag, paired);
  atomic_store(&stop_flag, false);
  if (pthread_create(&thread, NULL, run, NULL)) {
    return -1;
  }
  atomic_store(&started, true);
  return 0;
}

void mdns_reannounce(bool paired) {
  atomic_store(&paired_flag, paired);
  atomic_store(&reannounce, true);
}

void mdns_stop(void) {
  if (!atomic_load(&started)) {
    return;
  }
  atomic_store(&stop_flag, true);
  pthread_join(thread, NULL);
  atomic_store(&started, false);
}

bool mdns_running(void) { return atomic_load(&running); }
