/* usb_fast_sim.c: the two calculators' USB controllers for the fast
 * transport's cases in usb_test.c. */
#ifndef USB_FAST_SIM_H
#define USB_FAST_SIM_H
#include <stdint.h>

enum { SIM_CX2_HOST = 1, SIM_CX_DEVICE, SIM_CX2_DEVICE, SIM_CX_HOST };
enum { SIM_FAULT_NONE, SIM_FAULT_NOANSWER, SIM_FAULT_STALL, SIM_FAULT_PULL, SIM_FAULT_VIC_ON, SIM_FAULT_SLOW_TAKE,
       SIM_FAULT_SLOW_RELEASE, SIM_FAULT_SLOW_RESTART };

void usb_fast_sim_setup(int model_a, int model_b);
/* From now: the fault, after this many packets on our endpoint (stall,
 * pull) or at once (no answer). */
void usb_fast_sim_fault(int fault, int after);
void usb_fast_sim_cable(int in);
/* The host's OS has NavNet's ack to the device waiting (its endpoint 1 OUT
 * qTD active) or not. */
void usb_fast_sim_os_out(int host, int waiting);
/* The fake NavNet's: a side's OS can't take NavNet now; a side wrote. */
int usb_fast_sim_navnet_frozen(int reader);
void usb_fast_sim_navnet_wrote(int writer);
/* LINK2: the host's USBSTS reads since its release started the
 * OS's async schedule again (SIM_FAULT_SLOW_RESTART: ASS lags that start
 * 1,000,000 reads), -1 if it hasn't. */
long usb_fast_sim_restart_reads(int side);
/* NULL if both controllers are as their OSes left them. */
const char *usb_fast_sim_restored(void);
#endif
