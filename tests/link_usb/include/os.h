/* tests/link_usb's stand-in for Ndless's os.h: NavNet's calls (usb_test.c
 * fakes them), one set for each side (side.h renames them). */
#ifndef LINK_USB_TEST_OS_H
#define LINK_USB_TEST_OS_H
#include <stdint.h>
#include "side.h"

typedef struct fake_end *nn_ch_t;
typedef struct fake_node *nn_nh_t;
typedef struct fake_op *nn_oh_t;

#define e_TI_NN_Read 272
#define e_TI_NN_Write 273
#define __SYSCALLS_ISVAR 0x800000
#define nl_hassyscall(name) 1
uintptr_t link_usb_test_address(int number);
#define SYSCALL_ADDRESS(number) link_usb_test_address((number) & 0xFFFF)

nn_oh_t TI_NN_CreateOperationHandle(void);
int16_t TI_NN_DestroyOperationHandle(nn_oh_t oh);
int16_t TI_NN_NodeEnumInit(nn_oh_t oh);
int16_t TI_NN_NodeEnumNext(nn_oh_t oh, nn_nh_t *nh);
int16_t TI_NN_NodeEnumDone(nn_oh_t oh);
int16_t TI_NN_Connect(nn_nh_t nh, uint32_t service_id, nn_ch_t *ch);
int16_t TI_NN_Disconnect(nn_ch_t ch);
int16_t TI_NN_StartService(uint32_t service_id, void *data, void (*cb)(nn_ch_t ch, void *data));
int16_t TI_NN_StopService(uint32_t service_id);
#endif
