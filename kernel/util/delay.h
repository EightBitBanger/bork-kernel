#ifndef KERNEL_UTIL_DELAY_H
#define KERNEL_UTIL_DELAY_H

#include <stdint.h>

#ifdef KERNEL_PLATFORM_AVR
  
  #ifndef F_CPU
    #define F_CPU 20000000UL
  #endif
  
  // avr-libc's <util/delay.h>. This header must not itself be reachable as
  // <util/delay.h> on the include path, or it will shadow avr-libc's
  // (the include guard would then silently swallow it).
  #include <util/delay.h>
  
#endif

#ifdef KERNEL_PLATFORM_X86
    
    void delay_ms(uint32_t ms);
    
#endif

#endif
