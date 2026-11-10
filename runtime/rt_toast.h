/* On-screen notices (Linux, phase 10 of docs/linux.md): RetroAchievements' unlocks and messages, drawn over the
 * picture by the hardware renderer for a few seconds without pausing the game. Built only with GOW_HAVE_TOAST. */
#ifndef RT_TOAST_H
#define RT_TOAST_H

#include <stddef.h>
#include <stdint.h>

/* any thread: queues a notice. head: the small line on top (right: what goes at its right, or NULL), title: the
 * big one, text: under it, up to two lines. png: its icon (any size, copied), or NULL. */
void rt_toast_push(const char *head, const char *right, const char *title, const char *text, const void *png, size_t png_len);

/* the renderer, at each picture: the notice to show for a window win_h pixels high, w x h RGBA pixels, or NULL.
 * *alpha: its opacity now; *serial changes when the pixels do. */
const void *rt_toast_frame(int win_h, int *w, int *h, float *alpha, unsigned *serial);

#endif
