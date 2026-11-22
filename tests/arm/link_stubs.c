/* The link (nspire_link.c) the core calls into, unplugged: the replays run
 * one GBA, with nothing on its serial port. */
#include <stddef.h>
#include <stdint.h>

void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
void netpacket_poll_receive(void) {}
/* Called, not inlined, as the calculator's are (serial.h: noipa), so the
 * hot code the speed model runs is the calculator's. */
__attribute__((noipa)) void cable_transfer_start(uint16_t siocnt, uint32_t cycles) {}
__attribute__((noipa)) void cable_transfer_done(void) {}
__attribute__((noipa)) void cable_update(unsigned cycles) {}
__attribute__((noipa)) uint32_t cable_next_event(void) { return ~0u; }
