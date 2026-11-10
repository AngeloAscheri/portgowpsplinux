/* RetroAchievements (Linux, phase 10 of docs/linux.md): rcheevos' rc_client, softcore only. Without
 * GOW_HAVE_RCHEEVOS every function is empty. */
#ifndef RT_RA_H
#define RT_RA_H

#include <stdint.h>

/* --retroachievements FILE (user= and token= lines, tools/ra_login.py writes it), --ra-spectator (evaluate, send
 * nothing): 1 if argv[*i] was one of them */
int rt_ra_option(int argc, char **argv, int *i);
/* after the disc is open: signs in and loads the game's set; cheats (--warp, --load-level, --god, --input) force spectator mode */
void rt_ra_start(int cheats);
/* at every displayed frame: runs the achievements 60 times a second of game time (now_us: virtual microseconds) */
void rt_ra_frame(uint64_t now_us);
/* while the game waits (options menu): keeps the connection alive without reading memory */
void rt_ra_idle(void);
/* at exit: waits a moment for unlocks still being sent */
void rt_ra_shutdown(void);

/* The loaded set, for the lists (options menu): unlocked first, without the server's 0-point notes. icon: the
 * cached PNG's path (it may not be there yet). rt_ra_set returns 0 when no set is loaded. */
typedef struct { char title[96], desc[200], icon[600]; unsigned points; int unlocked; } RtRaEntry;
typedef struct { char game[96]; int n, have; unsigned pts, pts_have; RtRaEntry *e; } RtRaSet;
int rt_ra_set(RtRaSet *set);
void rt_ra_set_free(RtRaSet *set);

#endif
