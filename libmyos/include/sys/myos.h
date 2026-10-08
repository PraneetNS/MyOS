#ifndef _SYS_MYOS_H
#define _SYS_MYOS_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned int sys_free_frames(void);
unsigned int sys_ticks(void);
int sys_sync(void);
void print_uint(unsigned int n);
void print_hex(unsigned int v);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MYOS_H */
