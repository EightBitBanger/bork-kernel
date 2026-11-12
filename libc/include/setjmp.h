#ifndef _SETJMP_H
#define _SETJMP_H

/* i386: EBX, ESI, EDI, EBP, ESP and the return address (src/setjmp.c).
   No signals, so sigsetjmp/siglongjmp are the same thing. */
typedef unsigned long jmp_buf[6];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int value) __attribute__((noreturn));
int  _setjmp(jmp_buf env) __attribute__((returns_twice));
void _longjmp(jmp_buf env, int value) __attribute__((noreturn));

#define sigsetjmp(env, savemask)    setjmp(env)
#define siglongjmp(env, value)      longjmp(env, value)

#endif
