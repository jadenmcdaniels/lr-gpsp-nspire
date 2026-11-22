/* Bluetooth through a USB dongle, for controllers and headphones (r16, the
 * menus' side; docs/ui/spec.txt, the widget spec).
 *
 * The page (nspire_bluetooth_page, Settings > Bluetooth, under Controller in
 * the paused menu's list) is a list page (ui_list_page_draw):
 *   BLUETOOTH       On              a switch: enter flips it
 *   SEARCH          Searching       enter starts or stops a search
 *   PAIRED                          (each: enter connects or disconnects,
 *   <name>          Connected       del forgets it, after a question)
 *   <name>          Connecting      (enter stops it: "Stopped.")
 *   FOUND                           as a phone's list (r19-bt):
 *   <name>          Controller      
 *   <name>          Pairing         every named device with the word for
 *   <name>   Headphones . Weak      what it says it is (Controller,
 *   Example TV Set ...     TV       Headphones, Speaker, Phone, Computer,
 *   44:16:22:5A:7B:01 Controller    TV, Keyboard, Mouse; none when nothing
 * says); a nameless LE device only when it shows the HID service or a
 * gamepad appearance (a pad in pairing mode, by its address); controllers
 * and headphones first, then the rest, each the stronger first; the page's
 * lines (UI_PAGE_LINES) keep the strongest, never a paired one dropped.
 * Enter tries any of them (pairs it, or says "Not a controller or
 * headphones." when it turns out not to be one).
 * A signal is said only when it is weak (the lowest two of the four steps:
 * under -75 dBm): r19's "3/4" and "1/4" did not say what they meant to
 * anyone reading them. What the driver is doing (ops->status) is the
 * band's what-happened line while it says something.
 *
 * The Bluetooth glue fills nspire_bt with its functions; while it is
 * NULL the page isn't listed. On the PC, GPSP_HOST_BT=1 gives a stand-in
 * with two controllers (a DualSense, paired; a Switch Pro Controller),
 * earbuds and two other devices (tests/host/ui_wired.py, scene "bluetooth").
 *
 * Toasts over the game (nspire_osd_message, drawn in capitals): one form for
 * every device, nspire_bt_toast's: "Controller 2 connected (Bluetooth)",
 * "Controller 2 disconnected", "Headphones connected", "Headphones
 * disconnected"; at most 50 characters. */
#ifndef NSPIRE_BLUETOOTH_H
#define NSPIRE_BLUETOOTH_H

#include "common.h"

enum bt_kind { BT_CONTROLLER, BT_HEADPHONES, BT_OTHER };
/* r19-bt: what a device says it is, the word its line shows (a phone's list): NONE shows no word */
enum bt_what { BT_WHAT_NONE, BT_WHAT_CONTROLLER, BT_WHAT_HEADPHONES, BT_WHAT_SPEAKER, BT_WHAT_PHONE,
               BT_WHAT_COMPUTER, BT_WHAT_TV, BT_WHAT_KEYBOARD, BT_WHAT_MOUSE };
enum bt_state { BT_FOUND, BT_PAIRING, BT_PAIRED, BT_CONNECTING, BT_CONNECTED };

struct bt_device
{
  char name[48];          /* "" until its name is heard (the address shows) */
  char address[18];       /* "AA:BB:CC:00:00:06" */
  u8 kind;                /* enum bt_kind */
  u8 state;               /* enum bt_state: FOUND is not paired */
  s8 signal;              /* 0-4 bars, -1 not known */
  u8 le;                  /* Bluetooth Low Energy */
  u8 what;                /* enum bt_what (r19-bt) */
  s8 rssi;                /* dBm, 0 not known: the page's order within a group (r19-bt) */
};

struct nspire_bt_ops
{
  int (*present)(void);                       /* 1: a dongle is in */
  int (*on)(void);
  void (*set_on)(int on);
  int (*searching)(void);
  void (*search)(int start);                  /* start (1; 2: the page's own, again) or stop (0) a search */
  int (*devices)(struct bt_device *out, int max);   /* everything paired or heard, any order (the page sorts) */
  int (*pair)(const char *address);           /* 1: started */
  int (*connect)(const char *address, int on);   /* a paired one: connect (1) or disconnect (0) */
  int (*forget)(const char *address);
  const char *(*status)(void);                /* what it is doing now ("Pairing Pro Controller..."), or NULL */
  /* r19-bt: stops a pairing or a connect under way (a device PAIRING or CONNECTING: the player's, or an
   * automatic reconnect), 1: stopped; no automatic reconnect to it until it is asked again or calls by
   * itself. NULL: a library that can't (the page then does nothing on such a line). */
  int (*stop)(const char *address);
  /* r19-bt: why this device can't be connected here, one sentence for the band ("This dongle can't connect
   * Bluetooth LE controllers."), or NULL. NULL op: nothing to say. */
  const char *(*why_not)(const char *address);
  /* r19-bt (switched off and on while the dongle started): 1 asked on and still starting,
   * 2 asked off and still stopping (a start-up under way is finished first), 0 neither: the switch's tile says
   * Starting / Stopping. NULL: nothing to say. */
  int (*switching)(void);
};

/* NULL: no Bluetooth in this build (the page isn't listed) */
extern const struct nspire_bt_ops *nspire_bt;

void nspire_bluetooth_page(void);
/* What the paused menu's list says beside Bluetooth: "Off", "On", "On, 1 connected" */
const char *nspire_bt_summary(void);
/* The toast for a device connected or gone: kind (enum bt_kind), player (a controller's number, 0 none) */
void nspire_bt_toast(int kind, int player, int connected);
/* The same words, for whoever shows them elsewhere (at most 50 characters) */
void nspire_bt_toast_text(char *out, unsigned size, int kind, int player, int connected);

#endif
