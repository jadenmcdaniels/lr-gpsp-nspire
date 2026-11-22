/* gameplaySP: Fire Emblem's link cable, each GBA playing the other's end
 * (the TI-Nspire port's; GPL v2 or later, as the rest of gpSP).
 *
 * Fire Emblem: The Sacred Stones' Link Arena talks over the multiplayer
 * cable with Intelligent Systems' link library (fireemblem8u's
 * sio_core.c). The parent's timer 3 starts a transfer 136 x 1024 cycles
 * after each one ends (about two a frame); each GBA puts the next word of
 * its queue on the wire, 0x7FFF when it has nothing to say. The words make
 * packets: 0x4FFF, the length in halfwords, two checksums, the data. The
 * messages that matter are numbered and acknowledged, and sent again every
 * 38 frames until they are; nothing else counts the words' timing (a GBA
 * gives up only after more than 60 transfers in a row of 0xFFFF from the
 * other, a second with no transfer, or more than 60 sends of a message
 * never acknowledged). So the cable only has to carry the packets, in
 * order, however late. The Link Arena uses nothing else, from its lobby to
 * a battle's results (tests/link/scenarios/fire_emblem.py checks the
 * cable's every transfer).
 *
 * Here, as serial_proto.c does for Pokemon and Advance Wars, each gpSP
 * plays the other GBA's end: this game's packets go to the other gpSP
 * whole (netpacket_send) once it has put them on the wire, and the other's
 * come out of a queue, a word a transfer, 0x7FFF between them. Player 1's
 * game starts the transfers as on the GBA; player 2 gets them from a
 * pretend parent at the same pace. The two other plugs are empty (0xFFFF),
 * and so is the other GBA's while there's none (not linked). Linked, the
 * other's end says 0x7FFF even before its game comes to the arena: player
 * 1 waiting there alone shows player 2 "Connecting" (on the wire, "Not
 * Linked") until it comes. */

#include "common.h"

void netpacket_send(uint16_t client_id, const void *buf, size_t len);

#define FE_MAGIC        0x4d464531   /* MFE1: one packet's words */
#define FE_START        0x4FFF       /* a packet's first word */
#define FE_IDLE         0x7FFF       /* nothing to say */
#define FE_LONGEST      0x80         /* the longest length word the game takes */
#define FE_QUEUE        2048         /* words still to come (a power of two) */
/* Player 2's pretend parent: timer 3's wait, then a two-GBA transfer. */
#define FE_CYCLES       (136 * 1024 + 2 * 2621)

static struct
{
  u16 out[4 + FE_LONGEST];  /* this game's packet so far, as it went out */
  u32 out_len;              /* 0: between packets */
  u16 in[FE_QUEUE];         /* the other's words, from in_head to in_tail */
  u32 in_head, in_tail;
  u32 cycles;               /* player 2: since the pretend parent's last */
} fe;

void serialfe_reset(void)
{
  memset(&fe, 0, sizeof(fe));
}

/* A word this game put on the wire: a packet's goes into it, and the
 * packet to the other gpSP once whole. Words between packets (0x7FFF, the
 * hello's) aren't needed there. A 0x4FFF whose next word can't be a length
 * began a packet the game then dropped (it empties its queue between
 * steps): that word may start the next. */
static void put_word(u16 word)
{
  u8 msg[4 + 2 * (4 + FE_LONGEST)];
  u32 i;

  if (fe.out_len == 1 && word > FE_LONGEST)
    fe.out_len = 0;
  if (!fe.out_len && word != FE_START)
    return;
  fe.out[fe.out_len++] = word;
  if (fe.out_len < 4 || fe.out_len < 4u + fe.out[1])
    return;

  msg[0] = FE_MAGIC >> 24;
  msg[1] = (FE_MAGIC >> 16) & 0xFF;
  msg[2] = (FE_MAGIC >> 8) & 0xFF;
  msg[3] = FE_MAGIC & 0xFF;
  for (i = 0; i < fe.out_len; i++)
  {
    msg[4 + 2 * i] = fe.out[i] >> 8;
    msg[5 + 2 * i] = fe.out[i] & 0xFF;
  }
  netpacket_send(RETRO_NETPACKET_BROADCAST, msg, 4 + 2 * fe.out_len);
  fe.out_len = 0;
}

/* The other GBA's word for this transfer: its packets' next, or 0x7FFF;
 * 0xFFFF with no GBA there. */
static u16 other_word(void)
{
  u16 word;

  if (!netplay_client_id && !netplay_num_clients)
    return 0xFFFF;
  if (fe.in_head == fe.in_tail)
    return FE_IDLE;
  word = fe.in[fe.in_head];
  fe.in_head = (fe.in_head + 1) & (FE_QUEUE - 1);
  return word;
}

/* Player 1's game started a transfer (serial.c write_siocnt): the words
 * all four GBAs read at its end. */
void serialfe_master_send(void)
{
  u16 word = read_ioreg(REG_SIOMLT_SEND);

  put_word(word);
  write_ioreg(REG_SIOMULTI0, word);
  write_ioreg(REG_SIOMULTI1, other_word());
  write_ioreg(REG_SIOMULTI2, 0xFFFF);
  write_ioreg(REG_SIOMULTI3, 0xFFFF);
}

/* Player 2: the pretend parent's transfer, in multiplayer mode only (as a
 * parent's reaches a child); its interrupt if the game asked for one. */
static __attribute__((noinline)) bool serialfe_update_child(unsigned cycles)
{
  u16 siocnt = read_ioreg(REG_SIOCNT), word;

  fe.cycles += cycles;
  if (fe.cycles < FE_CYCLES)
    return false;
  fe.cycles = 0;
  if ((read_ioreg(REG_RCNT) & 0x8000) || (siocnt & 0x3000) != 0x2000)
    return false;
  word = read_ioreg(REG_SIOMLT_SEND);
  put_word(word);
  write_ioreg(REG_SIOMULTI0, other_word());
  write_ioreg(REG_SIOMULTI1, word);
  write_ioreg(REG_SIOMULTI2, 0xFFFF);
  write_ioreg(REG_SIOMULTI3, 0xFFFF);
  return (siocnt & 0x4000) != 0;
}

/* At every event (serial.c update_serial); out of line, so the code every
 * event runs stays small (common.h NSPIRE_HOT). */
NSPIRE_HOT bool serialfe_update(unsigned cycles)
{
  if (!netplay_client_id)
    return false;
  return serialfe_update_child(cycles);
}

/* A packet from the other gpSP (nspire_link.c, at a frame's start): its
 * words join the queue, or are dropped whole if they don't fit (the game
 * sends what matters again). */
void serialfe_net_receive(const void *buf, size_t len, uint16_t client_id)
{
  const u8 *p = buf;
  u32 words = (len - 4) / 2, i;

  if (len < 4 + 2 * 4 || (len & 1) || words > 4 + FE_LONGEST ||
      ((u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) != FE_MAGIC)
    return;
  if (((fe.in_head - fe.in_tail - 1) & (FE_QUEUE - 1)) < words)
    return;
  for (i = 0; i < words; i++)
  {
    fe.in[fe.in_tail] = p[4 + 2 * i] << 8 | p[5 + 2 * i];
    fe.in_tail = (fe.in_tail + 1) & (FE_QUEUE - 1);
  }
}
