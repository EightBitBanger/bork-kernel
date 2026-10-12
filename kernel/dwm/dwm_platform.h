#ifndef DWM_PLATFORM_H
#define DWM_PLATFORM_H

#ifdef KERNEL_PLATFORM_X86
  #include <kernel/arch/x86/heap.h>
  #include <kernel/arch/x86/drivers/display/draw.h>
#endif

#endif
