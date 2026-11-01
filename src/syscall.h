#ifndef SYSCALL_H
#define SYSCALL_H

struct registers;

void syscall_install(void);
void syscall_dispatch(struct registers* raw_regs);

#endif
