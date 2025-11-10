// kernel/start.c
#include "riscv.h"
#include "trap.h"
#include <stdio.h>

// Provided by uart.c.
extern void uart_init(void);

// Supervisor-mode entry point implemented in main.c.
void kmain(void);

void start(void) {
  // Bring up the serial console first so that subsequent messages are visible.
  uart_init();

  // Configure the supervisor trap handling code before we enable interrupts.
  trap_init();

  // Enter the main kernel routine. It should never return.
  kmain();

  // If it does return, park the hart in a low-power wait loop.
  while (1) {
    asm volatile("wfi");
  }
}
