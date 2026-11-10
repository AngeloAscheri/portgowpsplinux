/* The options a player can change, shared by the in-game menu (rt_host.c) and the game picker (gow_menu.c), so
 * both read and write the same --settings file: one "key=value" line per option, and a value must be one of the
 * option's own. */
#ifndef RT_SETTINGS_H
#define RT_SETTINGS_H

typedef struct { int res, fps, vsync, msaa, fxaa, aniso, sharpen, aspect, crt, fullscreen, hud_smooth; } Settings;

static const int RES_V[] = { 1, 2, 3, 4, 6, 8 }, FPS_V[] = { 60, 120, 144, 165, 240 }, BOOL_V[] = { 0, 1 }, MSAA_V[] = { 1, 2, 4, 8 },
                 ANISO_V[] = { 1, 2, 4, 8, 16 }, SHARPEN_V[] = { 0, 25, 50, 75, 100 }, ASPECT_V[] = { 0, 1, 2, 3 };
static const char *const RES_N[] = { "x1 480x272", "x2 960x544", "x3 1440x816", "x4 1920x1088", "x6 2880x1632", "x8 3840x2176" };
static const char *const FPS_N[] = { "60", "120", "144", "165", "240" };
static const char *const BOOL_N[] = { "No", "Si" };
static const char *const MSAA_N[] = { "No", "2x", "4x", "8x" };
static const char *const ANISO_N[] = { "No", "2x", "4x", "8x", "16x" };
static const char *const SHARPEN_N[] = { "No", "25%", "50%", "75%", "100%" };
static const char *const ASPECT_N[] = { "Original", "21:9", "32:9", "4:3" };

/* X(label, key, Settings field, values, names), in menu order */
#define RT_SETTINGS_ROWS(X) \
    X("Resolucion", "res_scale", res, RES_V, RES_N) \
    X("FPS", "fps", fps, FPS_V, FPS_N) \
    X("V-Sync", "vsync", vsync, BOOL_V, BOOL_N) \
    X("MSAA (al reiniciar)", "msaa", msaa, MSAA_V, MSAA_N) \
    X("FXAA", "fxaa", fxaa, BOOL_V, BOOL_N) \
    X("Anisotropico", "aniso", aniso, ANISO_V, ANISO_N) \
    X("Nitidez", "sharpen", sharpen, SHARPEN_V, SHARPEN_N) \
    X("Aspecto", "aspect", aspect, ASPECT_V, ASPECT_N) \
    X("Filtro CRT", "crt", crt, BOOL_V, BOOL_N) \
    X("Suavizar HUD", "hud_smooth", hud_smooth, BOOL_V, BOOL_N) \
    X("Pantalla completa", "fullscreen", fullscreen, BOOL_V, BOOL_N)

#endif
