/* Serial-MultiSio: a link cable stand-in for games on Nintendo's MultiSio
 * library (the AGB SDK's multi_sio.c), such as Golden Sun.
 *
 * On the cable the parent's timer starts one packet's transfers a frame: a
 * sync word (0xFEFE) and the packet (frame counter and receive errors,
 * checksum, data; Golden Sun's is 18 words, every 14,400 cycles). Each GBA's
 * interrupt takes all the GBAs' words and gives the next of its own, so the
 * other side's word is needed at every transfer: as wired
 * (SERIAL_MODE_CABLE) that's a message there and back each, 19 a frame.
 *
 * Here the cable runs four frames late instead, two each way (frames go
 * LINK_DELAY late, nspire_link.c). Player 1 (the parent) keeps each frame's
 * words, with the cycle of the frame each started at, and sends them at the
 * frame's end. Player 2 has no parent: they come at the start of its frame
 * two later, and it plays them to its game at the same cycles of that frame
 * (the same words, as far apart, its own interrupts), so they're over by its
 * end, and sends back the words its game gave. Those come at the start of
 * player 1's frame two after that, and its transfers take them one each, in
 * order: every word player 2's game gave, once (a frame with a transfer
 * fewer, as when the game restarts the library mid-packet, leaves one for
 * the next, as the other GBA's next word waits on the cable). So each game
 * sees the other's packets whole and in order, four frames late; Golden
 * Sun's steps each wait for the other side's count or state,
 * so that only delays them. Until the first answers come, player 1's game
 * hears nothing (0xFFFF), as with the other GBA not yet linking; and again
 * once its game has stopped for a few frames (what's left is from before).
 *
 * Packets (netpacket_send, the end of the frame they're for): "MSI1", the
 * number of words, the transfer's cycles, the words and (player 1's, not the
 * answers) the cycle each started at in its frame, 32 bits. */

#include "common.h"

void netpacket_send(uint16_t client_id, const void *buf, size_t len);

#define MSIO_HEADER      0x4d534931   /* MSI1 */
/* A frame's transfers at most: one takes 5242 cycles at 115200 bps, so 54. */
#define MSIO_WORDS       64
/* Player 1's frames on their way to player 2's game: one, or a few after a
 * state's loaded. */
#define MSIO_FRAMES      4
/* Player 2's answers waiting for player 1's transfers: a frame's, and what
 * frames with fewer transfers than four before left (a few). */
#define MSIO_ANSWERS     256
/* Frames without transfers after which player 1's answers left are old:
 * the last ones come four frames after their transfers. */
#define MSIO_IDLE        4

struct msio_frame
{
  u16 count;
  u16 time;                  /* a transfer's cycles (the baud rate's) */
  u16 words[MSIO_WORDS];
  u32 at[MSIO_WORDS];        /* each one's cycle in its frame */
};

static struct
{
  u32 frame_start;           /* cpu_ticks at this frame's start */

  /* Player 1: this frame's transfers, player 2's answers to come (a ring;
   * the last answers_new came at this frame's start), and the frames since
   * its game's last transfer. */
  struct msio_frame out;
  u16 answers[MSIO_ANSWERS];
  u32 answers_head, answers_count, answers_new;
  u32 idle;

  /* Player 2: player 1's frames to play (a ring; the first late of them
   * are from before this frame: their words go at once), the next word of
   * the first, and its game's answers. */
  struct msio_frame in[MSIO_FRAMES];
  u32 in_head, in_count, late;
  u32 pos;
  struct msio_frame reply;
} msio;

void serialmsio_reset(void)
{
  memset(&msio, 0, sizeof(msio));
  msio.frame_start = cpu_ticks;
}

static void msio_send(const struct msio_frame *f, int at)
{
  u8 pkt[8 + 6 * MSIO_WORDS];
  u32 i, len = 8;

  pkt[0] = MSIO_HEADER >> 24;
  pkt[1] = (MSIO_HEADER >> 16) & 0xFF;
  pkt[2] = (MSIO_HEADER >> 8) & 0xFF;
  pkt[3] = MSIO_HEADER & 0xFF;
  pkt[4] = f->count;
  pkt[5] = f->count >> 8;
  pkt[6] = f->time;
  pkt[7] = f->time >> 8;
  for (i = 0; i < f->count; i++, len += 2)
  {
    pkt[len] = f->words[i];
    pkt[len + 1] = f->words[i] >> 8;
  }
  for (i = 0; at && i < f->count; i++, len += 4)
  {
    pkt[len] = f->at[i];
    pkt[len + 1] = f->at[i] >> 8;
    pkt[len + 2] = f->at[i] >> 16;
    pkt[len + 3] = f->at[i] >> 24;
  }
  netpacket_send(RETRO_NETPACKET_BROADCAST, pkt, len);
}

void serialmsio_net_receive(const void *buf, size_t len, uint16_t client_id)
{
  const u8 *p = (const u8 *)buf;
  u32 count, i, at = netplay_client_id != 0;   /* player 2 gets frames */
  struct msio_frame *f;

  if (len < 8 || p[0] != (u8)(MSIO_HEADER >> 24) || p[1] != (u8)(MSIO_HEADER >> 16) ||
      p[2] != (u8)(MSIO_HEADER >> 8) || p[3] != (u8)MSIO_HEADER)
    return;
  count = p[4] | (p[5] << 8);
  if (!count || count > MSIO_WORDS || len != 8 + (at ? 6 : 2) * count ||
      (at && msio.in_count == MSIO_FRAMES))
    return;
  if (!at)
  {
    /* Player 1: one comes at a frame's start, for its frame four before. A
     * second one means the one before it came a frame late (player 2 was
     * behind: a state loaded): it's dropped, or the answers would be a frame
     * later from now on. */
    msio.answers_count -= msio.answers_new;
    msio.answers_new = 0;
    for (i = 0; i < count && msio.answers_count < MSIO_ANSWERS; i++, msio.answers_new++)
      msio.answers[(msio.answers_head + msio.answers_count++) % MSIO_ANSWERS] =
        p[8 + 2 * i] | (p[9 + 2 * i] << 8);
    return;
  }
  f = &msio.in[(msio.in_head + msio.in_count++) % MSIO_FRAMES];
  f->count = count;
  f->time = p[6] | (p[7] << 8);
  for (i = 0; i < count; i++)
  {
    const u8 *a = p + 8 + 2 * count + 4 * i;
    f->words[i] = p[8 + 2 * i] | (p[9 + 2 * i] << 8);
    f->at[i] = a[0] | (a[1] << 8) | (a[2] << 16) | ((u32)a[3] << 24);
  }
}

/* Player 1's game starts a transfer (time: its cycles). Its word goes in
 * the frame's, and player 2's word is the next answer. */
void serialmsio_master_send(u32 time)
{
  struct msio_frame *f = &msio.out;
  u16 word = read_ioreg(REG_SIOMLT_SEND), answer = 0xFFFF;

  if (f->count < MSIO_WORDS)
  {
    f->time = time;
    f->words[f->count] = word;
    f->at[f->count++] = cpu_ticks - msio.frame_start;
    if (msio.answers_count)
    {
      answer = msio.answers[msio.answers_head];
      msio.answers_head = (msio.answers_head + 1) % MSIO_ANSWERS;
      msio.answers_count--;
    }
  }
  write_ioreg(REG_SIOMULTI0, word);
  write_ioreg(REG_SIOMULTI1, answer);
  write_ioreg(REG_SIOMULTI2, 0xFFFF);
  write_ioreg(REG_SIOMULTI3, 0xFFFF);
}

/* A frame's end (run_frame). Player 1: its transfers go; the answers left
 * are the next frame's first, but MSIO_IDLE frames after its game's last
 * transfer. Player 2: a frame not played yet (a state loaded) goes at once
 * from now on. */
void serialmsio_frame_update(void)
{
  if (!netplay_client_id)
  {
    if (msio.out.count)
      msio_send(&msio.out, 1);
    msio.idle = msio.out.count ? 0 : msio.idle + 1;
    if (msio.idle >= MSIO_IDLE)
      msio.answers_count = 0;
    msio.out.count = 0;
    msio.answers_new = 0;
  }
  else
    msio.late = msio.in_count;
  msio.frame_start = cpu_ticks;
}

/* Player 2: the next of player 1's words is due (late: cycles past it).
 * Its game gets it and gives its own, as at a parent's transfer, and the
 * transfer ends (the interrupt) the parent's time after it started; a whole
 * frame's answers go back. Not in multiplayer mode, the game takes no part
 * (0xFFFF, as give_word). */
static __attribute__((noinline)) void serialmsio_play(unsigned cycles, u32 late)
{
  struct msio_frame *f = &msio.in[msio.in_head];
  u16 siocnt = read_ioreg(REG_SIOCNT);
  int multi = !(read_ioreg(REG_RCNT) & 0x8000) && (siocnt & 0x3000) == 0x2000;
  u16 mine = multi ? read_ioreg(REG_SIOMLT_SEND) : 0xFFFF;

  if (multi)
  {
    write_ioreg(REG_SIOMULTI0, f->words[msio.pos]);
    write_ioreg(REG_SIOMULTI1, mine);
    write_ioreg(REG_SIOMULTI2, 0xFFFF);
    write_ioreg(REG_SIOMULTI3, 0xFFFF);
    serial_cable_busy(f->time + cycles - MIN(late, cycles));
  }
  msio.reply.words[msio.pos++] = mine;
  if (msio.pos < f->count)
    return;
  msio.reply.count = f->count;
  msio.reply.time = f->time;
  msio_send(&msio.reply, 0);
  msio.in_head = (msio.in_head + 1) % MSIO_FRAMES;
  msio.in_count--;
  msio.late -= msio.late != 0;
  msio.pos = 0;
}

/* At every event while no transfer is under way (update_serial); only
 * player 2 does anything: the next word once this frame's at its cycle. */
NSPIRE_HOT void serialmsio_update(unsigned cycles)
{
  s32 due;

  if (!netplay_client_id || !msio.in_count)
    return;
  due = msio.late ? 0 : (s32)(cpu_ticks - msio.frame_start - msio.in[msio.in_head].at[msio.pos]);
  if (due >= 0)
    serialmsio_play(cycles, due);
}
