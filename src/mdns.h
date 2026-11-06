/* mDNS announcement _ps5-hass._tcp.
 * mdns.c on the PS5 (own thread, microdns); mdns_stub.c on the host.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_MDNS_H
#define PS5H_MDNS_H

#include <stdbool.h>
#include <stdint.h>

int mdns_start(const char *device_id, uint16_t port, bool paired);
/* Announce again now (wake-up, address change, pairing change). */
void mdns_reannounce(bool paired);
void mdns_stop(void);
bool mdns_running(void);

#endif
