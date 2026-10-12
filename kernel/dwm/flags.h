#ifndef WINDOW_FLAGS_H
#define WINDOW_FLAGS_H

#include <stdint.h>

#define DWM_WFLAG_REDRAW        0x01  // Redraw user window
#define DWM_WFLAG_REFRESH       0x02  // Draw window buffer to the back buffer
#define DWM_WFLAG_REDECORATE    0x04  // Redraw window styling and titlebar
#define DWM_WFLAG_KEEP_OPEN     0x08  // Set during CLOSE to cancel the destroy (dwm_window_cancel_close)

#endif
