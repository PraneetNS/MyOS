#ifndef _SIGSETJMP_H
#define _SIGSETJMP_H

#include <setjmp.h>
#include <signal.h>

typedef struct __sigjmp_buf_tag {
    jmp_buf jmpbuf;
    int mask_saved;
    sigset_t mask;
} sigjmp_buf[1];

#define sigsetjmp(env, savesigs) \
    (((env)[0].mask_saved = (savesigs)), \
     ((savesigs) ? sigprocmask(SIG_BLOCK, NULL, &(env)[0].mask) : 0), \
     setjmp((env)[0].jmpbuf))

static inline void siglongjmp(sigjmp_buf env, int val) {
    if (env[0].mask_saved) {
        sigprocmask(SIG_SETMASK, &env[0].mask, NULL);
    }
    longjmp(env[0].jmpbuf, val ? val : 1);
}

#endif /* _SIGSETJMP_H */
