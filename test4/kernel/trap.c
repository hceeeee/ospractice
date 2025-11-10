// kernel/trap.c
#include "riscv.h"
#include "sbi.h"
#include "trap.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define MAX_IRQ 64

static interrupt_handler_t ivt[MAX_IRQ];
static volatile uint64_t ticks = 0;
volatile int interrupt_count = 0;

static const int irq_priority[] = {
    SCAUSE_SUPERVISOR_TIMER,
    SCAUSE_SUPERVISOR_EXTERNAL,
    SCAUSE_SUPERVISOR_SOFTWARE,
};

extern bool should_yield(void);
extern void yield(void);

static inline bool valid_irq(int irq) {
  return irq >= 0 && irq < MAX_IRQ;
}

static inline uint64_t irq_to_sie_bit(int irq) {
  switch (irq) {
    case SCAUSE_SUPERVISOR_SOFTWARE:
      return SIE_SSIE;
    case SCAUSE_SUPERVISOR_TIMER:
      return SIE_STIE;
    case SCAUSE_SUPERVISOR_EXTERNAL:
      return SIE_SEIE;
    default:
      return 0;
  }
}

static inline uint64_t irq_to_sip_bit(int irq) {
  switch (irq) {
    case SCAUSE_SUPERVISOR_SOFTWARE:
      return SIP_SSIP;
    case SCAUSE_SUPERVISOR_TIMER:
      return SIP_STIP;
    case SCAUSE_SUPERVISOR_EXTERNAL:
      return SIP_SEIP;
    default:
      return 0;
  }
}

static bool dispatch_irq(int irq) {
  if (!valid_irq(irq)) {
    return false;
  }
  interrupt_handler_t handler = ivt[irq];
  if (handler) {
    handler();
    return true;
  }
  return false;
}

static int choose_irq(uint64_t scause) {
  if ((scause & SCAUSE_INTR_MASK) == 0) {
    return -1;
  }

  const int cause = (int)SCAUSE_CODE(scause);
  const uint64_t pending = r_sip() & r_sie();
  const uint64_t cause_mask = irq_to_sip_bit(cause);

  if (cause_mask && (pending & cause_mask)) {
    return cause;
  }

  for (size_t i = 0; i < sizeof(irq_priority) / sizeof(irq_priority[0]); ++i) {
    const int candidate = irq_priority[i];
    const uint64_t mask = irq_to_sip_bit(candidate);
    if (mask && (pending & mask)) {
      return candidate;
    }
  }

  return valid_irq(cause) ? cause : -1;
}

void register_interrupt(int irq, interrupt_handler_t handler) {
  if (valid_irq(irq)) {
    ivt[irq] = handler;
  }
}

void unregister_interrupt(int irq) {
  if (valid_irq(irq)) {
    ivt[irq] = NULL;
  }
}

void enable_interrupt(int irq) {
  const uint64_t mask = irq_to_sie_bit(irq);
  if (!mask) {
    return;
  }
  uint64_t sie = r_sie();
  sie |= mask;
  w_sie(sie);
}

void disable_interrupt(int irq) {
  const uint64_t mask = irq_to_sie_bit(irq);
  if (!mask) {
    return;
  }
  uint64_t sie = r_sie();
  sie &= ~mask;
  w_sie(sie);
}

uint64_t get_time(void) {
  return r_time();
}

#define TIMEBASE_HZ 10000000ULL
#define HZ          100ULL
#define TICK_CYCLES (TIMEBASE_HZ / HZ)

static void set_next_timer(void) {
  const uint64_t now = get_time();
  sbi_set_timer(now + TICK_CYCLES);
}

void timer_interrupt(void) {
  ++ticks;
  ++interrupt_count;

  if (should_yield()) {
    yield();
  }

  set_next_timer();
}

extern void kernelvec(void);

void trap_init(void) {
  intr_off();
  for (int i = 0; i < MAX_IRQ; ++i) {
    ivt[i] = NULL;
  }
  ticks = 0;
  interrupt_count = 0;

  w_sip(r_sip() & ~(SIP_SSIP | SIP_STIP | SIP_SEIP));
  w_stvec((uint64_t)kernelvec);

  register_interrupt(SCAUSE_SUPERVISOR_TIMER, timer_interrupt);
  enable_interrupt(SCAUSE_SUPERVISOR_TIMER);

  set_next_timer();
  intr_on();
}

int devintr(struct trapframe *tf) {
  const int irq = choose_irq(tf->scause);
  if (irq < 0) {
    return 0;
  }
  if (dispatch_irq(irq)) {
    return 1;
  }
  return 0;
}

void kerneltrap(struct trapframe *tf) {
  tf->sepc = r_sepc();
  tf->sstatus = r_sstatus();
  tf->stval = r_stval();
  tf->scause = r_scause();
  tf->reserved = 0;

  if (tf->sstatus & SSTATUS_SIE) {
    printf("kerneltrap: interrupts enabled\n");
  }

  if (tf->scause & SCAUSE_INTR_MASK) {
    if (!devintr(tf)) {
      printf("kerneltrap: unexpected interrupt cause=%lu\n",
             (unsigned long)SCAUSE_CODE(tf->scause));
    }
  } else {
    printf("kerneltrap: unexpected exception scause=%lu sepc=%#lx stval=%#lx\n",
           (unsigned long)tf->scause,
           (unsigned long)tf->sepc,
           (unsigned long)tf->stval);
    while (1) {
      asm volatile("wfi");
    }
  }

  w_sepc(tf->sepc);
}

void usertrap(struct trapframe *tf) {
  kerneltrap(tf);
}
