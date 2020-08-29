/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PARISC_IRQFLAGS_H
#define __PARISC_IRQFLAGS_H

#include <linux/types.h>
#include <asm/psw.h>

unsigned long arch_local_save_flags(void);
void arch_local_irq_disable(void);
void arch_local_irq_enable(void);
unsigned long arch_local_irq_save(void);
void arch_local_irq_restore(unsigned long flags);
bool arch_irqs_disabled_flags(unsigned long flags);
bool arch_irqs_disabled(void);

#endif /* __PARISC_IRQFLAGS_H */
