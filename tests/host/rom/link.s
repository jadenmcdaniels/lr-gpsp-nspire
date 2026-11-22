@ A GBA link test ROM for gpsp-host's link tests (make_link_rom.py holds it
@ as bytes): two of it on a multiplayer cable, the parent (player 1)
@ making 9 transfers a frame as Pokemon's does: one at vblank, then each a
@ timer 3 interrupt (12608 cycles) after the last one's end. Each side's
@ next word is the top half of a sum over all the words it got (sum = sum
@ * 33 + word + 1, at 0x03000000), so each word hangs on all the other
@ side's before it. Between interrupts the CPU halts (HALTCNT), or with A
@ held it spins instead, as a game waiting in a loop does.
    .arm
    .section .text
    .global _start
_start:
    b       main
    .space  0xC0 - 4            @ the cartridge header (gpSP doesn't check it)
main:
    ldr     r0, =0x04000100     @ the serial port's, timers' and keys' registers
    adr     r1, irq             @ where it runs (the ROM is at 0x08000000)
    ldr     r2, =0x03007FFC     @ the BIOS's interrupt handler calls it
    str     r1, [r2]
    mov     r1, #0
    strh    r1, [r0, #0x34]     @ RCNT: the serial port
    ldr     r1, =0x6003         @ SIOCNT: multiplayer, 115200 bps, interrupt
    strh    r1, [r0, #0x28]
    mov     r2, #0x04000000
    mov     r1, #0x08           @ DISPSTAT: vblank interrupt
    strh    r1, [r2, #4]
    add     r2, r2, #0x200
    mov     r1, #0xC1           @ IE: vblank, timer 3, serial
    strh    r1, [r2]
    mov     r1, #1
    strh    r1, [r2, #8]        @ IME
idle:
    ldrh    r1, [r0, #0x30]     @ KEYINPUT: A held (0) spins
    tst     r1, #1
    beq     idle
    mov     r1, #0
    strb    r1, [r0, #0x201]    @ HALTCNT: halt till an interrupt
    b       idle

@ The interrupts (IRQ mode, from the BIOS's handler).
irq:
    ldr     r0, =0x04000100
    add     r3, r0, #0x100
    ldrh    r1, [r3]            @ IE & IF, acknowledged
    ldrh    r2, [r3, #2]
    and     r1, r1, r2
    strh    r1, [r3, #2]
    mov     r12, #0x03000000
    tst     r1, #0x80
    bne     serial
    tst     r1, #0x40
    bne     timer3
    tst     r1, #0x01
    bxeq    lr
    @ vblank: the IDs (bit 2, SI: the child's 1); the parent starts
    ldr     r2, =0x6003
    strh    r2, [r0, #0x28]
    ldrh    r2, [r0, #0x28]
    tst     r2, #4
    bxne    lr
    mov     r2, #0
    str     r2, [r12, #4]       @ transfers this frame
start:
    ldr     r2, =0x6083         @ SIOCNT's start bit
    strh    r2, [r0, #0x28]
    bx      lr
timer3:
    mov     r2, #0
    strh    r2, [r0, #0x0E]     @ timer 3 off
    b       start
serial:
    ldrh    r3, [r0, #0x28]
    tst     r3, #4
    ldrneh  r2, [r0, #0x20]     @ the child takes SIOMULTI0, the parent's
    ldreqh  r2, [r0, #0x22]     @ the parent SIOMULTI1, the child's
    ldr     r1, [r12]
    add     r1, r1, r1, lsl #5
    add     r1, r1, r2
    add     r1, r1, #1
    str     r1, [r12]
    mov     r2, r1, lsr #16
    strh    r2, [r0, #0x2A]     @ SIOMLT_SEND: the next word
    tst     r3, #4
    bxne    lr
    ldr     r2, [r12, #4]
    add     r2, r2, #1
    str     r2, [r12, #4]
    cmp     r2, #9
    bxge    lr
    ldr     r2, =0xFF3B         @ timer 3: 197 ticks of 64 cycles
    strh    r2, [r0, #0x0C]
    mov     r2, #0xC2           @ on, interrupt, 64 cycles a tick
    strh    r2, [r0, #0x0E]
    bx      lr
    .pool
