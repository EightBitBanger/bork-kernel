#ifndef DWM_CONSENT_H
#define DWM_CONSENT_H

/*
 * The administrator prompt ("consent"): a program asked the kernel for
 * admin rights (sec_elevate, os/security.h) and the user says Allow or
 * Cancel here.
 *
 * The DWM registers with the kernel as the one elevation agent at start-up.
 * The kernel queues every request and hands the DWM one at a time
 * (sec_agent_next), with the program's name and path from its own process
 * table; the program only adds a line of its own words, shown as such.
 * The answer goes straight back to the kernel (sec_agent_answer), which
 * grants the rights itself. No program takes part in any of it: there is no
 * window, message or event for one to see, raise, move, cover or click.
 *
 * While a prompt is up:
 *   - it is painted over everything (windows, taskbar, menus, notifications,
 *     drags), with the rest of the screen dimmed, in fixed colors no theme
 *     or program can change
 *   - it takes every key and mouse event before anything else; menus, the
 *     start menu, drags, captures and pointer locks are ended when it opens
 *     and no pointer lock can start
 *   - Allow does nothing for its first CONSENT_ARM_MS (drawn greyed), a
 *     press must start and end on it, and only after every button was up
 *     once since the prompt appeared, so a click (or a held button) meant
 *     for something else can't land on it. The kernel enforces its own
 *     minimum too (SYS_ELEVATE_ARM_MS).
 *   - the keyboard starts on Cancel. Tab, Left and Right move between the
 *     buttons, Enter or Space presses the one chosen, Escape is Cancel.
 *     Until the prompt is armed only Escape works.
 *
 * Neither answer is followed by a notification: the user just chose it.
 *
 * Launches: a program can be started elevated (SPAWN_ELEVATE, os/process.h).
 * The kernel then asks with SYS_ELEVATE_LAUNCH: the prompt names the
 * program to be started, the process starting it and the permissions it
 * would get, one per line. The desktop's own
 * launches (icons, the start menu) ask with consent_ask_launch(), on the
 * same prompt, since the DWM can't wait for its own answer.
 */

#include <stdbool.h>
#include <stdint.h>
#include <os/gfx.h>

#define CONSENT_ARM_MS      1000    // Allow is dead this long after the prompt shows

// Become the elevation agent (dwm_init). Without SEC_ADMIN, SEC_INPUT and
// SEC_GFX_SCREEN, or when another process is the agent, there are simply
// no prompts: programs asking get EAGAIN.
void    consent_init(void);
void    consent_shutdown(void);         // An open prompt is answered Cancel

// Main loop: show the next request, drop one the program stopped waiting for
void    consent_tick(void);
int32_t consent_next_timeout(void);     // ms until the prompt needs a look, -1: none

bool    consent_active(void);           // A prompt is up: it owns the input

// Ask the user whether the program at `path` may start with the
// `requested` SYS_SEC_* bits, on the prompt (after one that is up now). `done` is called once from the
// main loop with the answer (false too when the DWM shuts down first).
// False with errno EBUSY when one of these is already waiting or up,
// EAGAIN when there are no prompts (consent_init failed).
typedef void (*consent_done_fn)(bool allow, void* ctx);
bool    consent_ask_launch(const char* path, uint32_t requested, consent_done_fn done, void* ctx);

// Input while consent_active() (input.c sends it everything first)
void    consent_mouse(int32_t x, int32_t y, uint8_t buttons);
void    consent_key(uint16_t key, uint8_t mods);

// Compositor: above everything but the cursor
void    consent_paint(gfx_surface_t* s, gfx_rect_t area);

#endif
