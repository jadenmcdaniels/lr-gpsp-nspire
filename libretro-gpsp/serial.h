/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define SERIAL_MODE_DISABLED      0
#define SERIAL_MODE_GBP           1  // Connected to the GB Player
#define SERIAL_MODE_RFU           2  // Wifi Adapter
#define SERIAL_MODE_SERIAL_POKE   3  // Connected to serial cable (Pokemon emulation mode)
#define SERIAL_MODE_SERIAL_AW1    4  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_SERIAL_AW2    5  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_AUTO          6  // Choose best fit automatically
#define SERIAL_MODE_CABLE         7  // A real link cable to another emulator (nspire_link.c)
#define SERIAL_MODE_SERIAL_FE     8  // Connected to serial cable (Fire Emblem emulation mode)
#define SERIAL_MODE_SERIAL_MSIO   9  // Connected to serial cable (Nintendo MultiSio library, serial_msio.c)

extern int serial_mode;
extern int cable_linked;
// nspire R4: transfers the game started, serial events that came (the link's log).
extern u32 serial_transfer_starts, serial_irqs_raised;

// Register writes
cpu_alert_type write_siocnt(u16 value);
cpu_alert_type write_rcnt(u16 value);

// Serial IRQ interface
u32 serial_next_event();
bool update_serial(unsigned cycles);

// RFU interface
void rfu_reset(void);
bool rfu_update(unsigned cycles);
u32 rfu_transfer(u32 value);
void rfu_frame_update(void);
void rfu_net_receive(const void* buf, size_t len, uint16_t client_id);

// Serial implementations
void serialproto_reset(void);

void serialpoke_frame_update(void);
void serialpoke_master_send(void);
bool serialpoke_update(unsigned cycles);
u32 serialpoke_states(void);
void serialpoke_net_receive(const void* buf, size_t len, uint16_t client_id);

void serialaw_frame_update(void);
void serialaw_master_send(void);
bool serialaw_update(unsigned cycles);
void serialaw_net_receive(const void* buf, size_t len, uint16_t client_id);

// Fire Emblem's (serial_fe.c)
void serialfe_reset(void);
void serialfe_master_send(void);
bool serialfe_update(unsigned cycles);
void serialfe_net_receive(const void* buf, size_t len, uint16_t client_id);

// The Nintendo MultiSio library's (serial_msio.c)
void serialmsio_reset(void);
void serialmsio_master_send(u32 time);
void serialmsio_frame_update(void);
void serialmsio_update(unsigned cycles);
void serialmsio_net_receive(const void* buf, size_t len, uint16_t client_id);

// Link cable interface (the frontend's: nspire_link.c). The parent's
// transfer start waits for the other GBA's word; the other GBA stops at
// that same cycle to give it, so both finish the transfer together.
// Never inlined (LTO would put the link into update_gba, which then differs
// between the calculator build and the speed model's, whose link is
// tests/arm/link_stubs.c, and spreads the hot code over more cache lines).
__attribute__((noipa)) void cable_transfer_start(u16 siocnt, u32 cycles);
__attribute__((noipa)) void cable_transfer_done(void);
__attribute__((noipa)) void cable_update(unsigned cycles);
__attribute__((noipa)) u32 cable_next_event(void);
void serial_cable_busy(u32 cycles);
u32 serial_busy_cycles(void);
// How long the halted CPU surely stays so (main.c), to answer sooner.
u32 halted_cycles(u32 serial_busy);

// GBP interface
u32 gbp_transfer(u32 value);


