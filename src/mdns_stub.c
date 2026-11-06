/* Host build: no mDNS.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns.h"

int mdns_start(const char *device_id, uint16_t port, bool paired) {
  (void)device_id;
  (void)port;
  (void)paired;
  return 0;
}

void mdns_reannounce(bool paired) { (void)paired; }
void mdns_stop(void) {}
bool mdns_running(void) { return false; }
