/* RetroAchievements (Linux, phase 10 of docs/linux.md) through rcheevos' rc_client (MIT), softcore only: the
 * port isn't an emulator RetroAchievements has reviewed for hardcore.
 *
 *  - The game is the disc's: MD5 of PSP_GAME/PARAM.SFO followed by PSP_GAME/SYSDIR/EBOOT.BIN, as rcheevos hashes a
 *    PSP disc (the European discs are registered, the US ones are not).
 *  - Memory: RetroAchievements' address 0 is the PSP's 0x08000000 (kernel RAM, then user RAM at 0x00800000 and the
 *    extended RAM at 0x02000000), read straight from the port's PSP memory.
 *  - rc_client_do_frame runs once per 1/59.94 s of game time, whatever --fps is, so that conditions counting frames
 *    keep their meaning at 144 Hz.
 *  - The network goes through the system's libcurl, loaded with dlopen (Bazzite and SteamOS have it), one thread per
 *    request; without it the game runs without achievements.
 *  - --warp, --load-level, --god and --input replays only watch (spectator mode): no unlock is sent.
 *  - On screen (rt_toast.c, with the hardware renderer): unlocks, the set when it loads, sign-in and connection
 *    problems. Their icons are downloaded once into ra_images/ next to the --retroachievements file.
 *  - The set, unlocked or not, goes to achievements.txt next to the --settings file, for the game picker's list
 *    (gow_menu.c), when the set loads and at each unlock. Spectator runs leave it alone, unless RT_TEST_RA_SUMMARY
 *    names another file. */
#include "rt_ra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(GOW_HAVE_RCHEEVOS)
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include "psp_rt.h"
#include "rt_gpu.h"
#include "rc_client.h"
#include "md5.h"
#ifdef GOW_HAVE_TOAST
#include "rt_toast.h"
#endif

uint8_t *rt_io_read_disc_file(const char *disc_path, uint32_t *size);

#define USER_AGENT "GoWPSPRecomp/0.1 (Linux)"
#define FRAME_US 16683u  /* 1/59.94 s */

static rc_client_t *g_client;
static char g_user[128], g_token[128], g_hash[33];
static int g_spectator, g_logged_in;
static uint64_t g_next_frame;
static const char *g_ini;
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_requests;  /* in flight */
static char g_img_dir[512];  /* the icons' cache */
static uint64_t g_login_retry_ms;  /* without a connection at sign-in: when to try again (monotonic ms), 0 never */
static int g_offline_told;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* --- libcurl, loaded at run time -------------------------------------------------- */
enum { OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018, OPT_HTTPHEADER = 10023,
       OPT_WRITEFUNCTION = 20011, OPT_TIMEOUT = 13, OPT_FOLLOWLOCATION = 52, OPT_NOSIGNAL = 99, INFO_RESPONSE_CODE = 0x200002 };
static struct {
    void *lib;
    int (*global_init)(long);
    void *(*easy_init)(void);
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void (*easy_cleanup)(void *);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
} curl;

static int curl_load(void) {
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !curl.lib; i++) curl.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!curl.lib) return 0;
#define SYM(field, name) *(void **)&curl.field = dlsym(curl.lib, name)
    SYM(global_init, "curl_global_init"); SYM(easy_init, "curl_easy_init"); SYM(easy_setopt, "curl_easy_setopt");
    SYM(easy_perform, "curl_easy_perform"); SYM(easy_getinfo, "curl_easy_getinfo"); SYM(easy_cleanup, "curl_easy_cleanup");
    SYM(slist_append, "curl_slist_append"); SYM(slist_free_all, "curl_slist_free_all");
#undef SYM
    if (!curl.global_init || !curl.easy_init || !curl.easy_setopt || !curl.easy_perform || !curl.easy_getinfo ||
        !curl.easy_cleanup || !curl.slist_append || !curl.slist_free_all) return 0;
    return curl.global_init(3) == 0;  /* CURL_GLOBAL_DEFAULT */
}

typedef struct { char *data; size_t len; } Buf;
static size_t on_body(char *p, size_t size, size_t n, void *user) {
    Buf *b = (Buf *)user;
    char *grown = (char *)realloc(b->data, b->len + size * n + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, p, size * n);
    b->data = grown;
    b->len += size * n;
    b->data[b->len] = 0;
    return size * n;
}

typedef struct { char *url, *post, *type; rc_client_server_callback_t cb; void *cb_data; } Request;

static void *request_thread(void *arg) {
    Request *r = (Request *)arg;
    Buf body = { NULL, 0 };
    long status = 0;
    void *h = curl.easy_init(), *headers = NULL;
    if (h) {
        curl.easy_setopt(h, OPT_URL, r->url);
        curl.easy_setopt(h, OPT_USERAGENT, USER_AGENT);
        curl.easy_setopt(h, OPT_NOSIGNAL, 1L);
        curl.easy_setopt(h, OPT_TIMEOUT, 30L);
        curl.easy_setopt(h, OPT_WRITEFUNCTION, on_body);
        curl.easy_setopt(h, OPT_WRITEDATA, &body);
        if (r->post) {
            char ct[160];
            snprintf(ct, sizeof(ct), "Content-Type: %s", r->type ? r->type : "application/x-www-form-urlencoded");
            headers = curl.slist_append(NULL, ct);
            curl.easy_setopt(h, OPT_HTTPHEADER, headers);
            curl.easy_setopt(h, OPT_POSTFIELDS, r->post);
        }
        if (curl.easy_perform(h) == 0) curl.easy_getinfo(h, INFO_RESPONSE_CODE, &status);
        if (headers) curl.slist_free_all(headers);
        curl.easy_cleanup(h);
    }
    rc_api_server_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body = body.data ? body.data : "";
    resp.body_length = body.len;
    resp.http_status_code = status ? (int)status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    r->cb(&resp, r->cb_data);
    free(body.data);
    free(r->url); free(r->post); free(r->type); free(r);
    pthread_mutex_lock(&g_req_lock);
    g_requests--;
    pthread_mutex_unlock(&g_req_lock);
    return NULL;
}

static void server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *cb_data, rc_client_t *client) {
    (void)client;
    Request *r = (Request *)calloc(1, sizeof(*r));
    r->url = strdup(req->url);
    r->post = req->post_data ? strdup(req->post_data) : NULL;
    r->type = req->content_type ? strdup(req->content_type) : NULL;
    r->cb = cb;
    r->cb_data = cb_data;
    pthread_mutex_lock(&g_req_lock);
    g_requests++;
    pthread_mutex_unlock(&g_req_lock);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, request_thread, r) != 0) {
        pthread_mutex_lock(&g_req_lock);
        g_requests--;
        pthread_mutex_unlock(&g_req_lock);
        rc_api_server_response_t resp = { "", 0, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR };
        cb(&resp, cb_data);
        free(r->url); free(r->post); free(r->type); free(r);
    }
    pthread_attr_destroy(&a);
}

/* --- icons and on-screen notices --------------------------------------------------- */
/* a GET on the calling thread: the HTTP status, 0 without an answer */
static long http_get(const char *url, Buf *body) {
    long status = 0;
    void *h = curl.easy_init();
    if (!h) return 0;
    curl.easy_setopt(h, OPT_URL, url);
    curl.easy_setopt(h, OPT_USERAGENT, USER_AGENT);
    curl.easy_setopt(h, OPT_NOSIGNAL, 1L);
    curl.easy_setopt(h, OPT_TIMEOUT, 15L);
    curl.easy_setopt(h, OPT_FOLLOWLOCATION, 1L);
    curl.easy_setopt(h, OPT_WRITEFUNCTION, on_body);
    curl.easy_setopt(h, OPT_WRITEDATA, body);
    if (curl.easy_perform(h) == 0) curl.easy_getinfo(h, INFO_RESPONSE_CODE, &status);
    curl.easy_cleanup(h);
    return status;
}

/* the PNG at url, from the cache or downloaded into it (malloc'd), or NULL */
static uint8_t *image(const char *url, size_t *len) {
    const char *name = strrchr(url, '/');
    name = name ? name + 1 : url;
    size_t n = strlen(name);
    int cache = g_img_dir[0] && n > 4 && n < 64 && !strcmp(name + n - 4, ".png");
    for (size_t i = 0; cache && i < n; i++) cache = isalnum((unsigned char)name[i]) || strchr("._-", name[i]);
    char path[640];
    if (cache) {
        snprintf(path, sizeof(path), "%s/%s", g_img_dir, name);
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t *data = NULL;
            long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
            if (size > 8 && size < (1 << 22) && fseek(f, 0, SEEK_SET) == 0 && (data = (uint8_t *)malloc((size_t)size)) &&
                fread(data, 1, (size_t)size, f) == (size_t)size) {
                fclose(f);
                *len = (size_t)size;
                return data;
            }
            free(data);
            fclose(f);
        }
    }
    Buf b = { NULL, 0 };
    if (!curl.easy_init) return NULL;
    if (http_get(url, &b) != 200 || b.len < 8 || memcmp(b.data, "\x89PNG", 4)) { free(b.data); return NULL; }
    if (cache) {
        char tmp[680];
        snprintf(tmp, sizeof(tmp), "%s.%lx.part", path, (unsigned long)pthread_self());
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(b.data, 1, b.len, f) == b.len;
            if (fclose(f) == 0 && ok) rename(tmp, path);
            else remove(tmp);
        }
    }
    *len = b.len;
    return (uint8_t *)b.data;
}

#ifdef GOW_HAVE_TOAST
typedef struct { char head[64], right[48], title[192], text[400], url[512]; } Notice;

static void *notice_thread(void *arg) {
    Notice *n = (Notice *)arg;
    size_t len = 0;
    uint8_t *png = n->url[0] ? image(n->url, &len) : NULL;
    rt_toast_push(n->head, n->right, n->title, n->text, png, len);
    free(png);
    free(n);
    return NULL;
}
#endif

/* on screen: with an icon, after it is fetched (another thread) */
static void notice(const char *head, const char *right, const char *title, const char *text, const char *url) {
#ifdef GOW_HAVE_TOAST
    Notice *n = (Notice *)calloc(1, sizeof(*n));
    if (!n) return;
    snprintf(n->head, sizeof(n->head), "%s", head ? head : "");
    snprintf(n->right, sizeof(n->right), "%s", right ? right : "");
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    snprintf(n->text, sizeof(n->text), "%s", text ? text : "");
    snprintf(n->url, sizeof(n->url), "%s", url ? url : "");
    if (!n->url[0]) { notice_thread(n); return; }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, notice_thread, n) != 0) { n->url[0] = 0; notice_thread(n); }
    pthread_attr_destroy(&a);
#else
    (void)head; (void)right; (void)title; (void)text; (void)url;
#endif
}

/* the set's icons into the cache, one after the other, so that an unlock shows its icon at once */
typedef struct { int n; char url[][256]; } Prefetch;
static void *prefetch_thread(void *arg) {
    Prefetch *p = (Prefetch *)arg;
    int got = 0;
    for (int i = 0; i < p->n; i++) {
        size_t len = 0;
        uint8_t *png = image(p->url[i], &len);
        got += png != NULL;
        free(png);
    }
    printf("[ra] %d of %d icons ready in %s\n", got, p->n, g_img_dir);
    free(p);
    return NULL;
}

static void prefetch_icons(rc_client_t *client) {
    if (!g_img_dir[0]) return;
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                           RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return;
    int total = 0;
    for (uint32_t b = 0; b < list->num_buckets; b++) total += (int)list->buckets[b].num_achievements;
    Prefetch *p = total ? (Prefetch *)calloc(1, sizeof(Prefetch) + (size_t)total * 256) : NULL;
    for (uint32_t b = 0; p && b < list->num_buckets; b++)
        for (uint32_t k = 0; k < list->buckets[b].num_achievements; k++)
            if (rc_client_achievement_get_image_url(list->buckets[b].achievements[k], RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,
                                                    p->url[p->n], 256) == RC_OK) p->n++;
    rc_client_destroy_achievement_list(list);
    if (!p) return;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, prefetch_thread, p) != 0) free(p);
    pthread_attr_destroy(&a);
}

/* --- the set, for the lists ------------------------------------------------------------ */
static int set_from(rc_client_t *client, RtRaSet *set) {
    memset(set, 0, sizeof(*set));
    if (!client || !rc_client_is_game_loaded(client)) return 0;
    const rc_client_game_t *g = rc_client_get_game_info(client);
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                           RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return 0;
    int total = 0;
    for (uint32_t b = 0; b < list->num_buckets; b++) total += (int)list->buckets[b].num_achievements;
    set->e = total ? (RtRaEntry *)calloc((size_t)total, sizeof(RtRaEntry)) : NULL;
    snprintf(set->game, sizeof(set->game), "%s", g && g->title ? g->title : "");
    for (int pass = 1; set->e && pass >= 0; pass--)  /* unlocked, then locked */
        for (uint32_t b = 0; b < list->num_buckets; b++)
            for (uint32_t k = 0; k < list->buckets[b].num_achievements; k++) {
                const rc_client_achievement_t *a = list->buckets[b].achievements[k];
                int unlocked = (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != 0;
                if (unlocked != pass || (!a->points && !strncmp(a->title, "Warning:", 8))) continue;
                RtRaEntry *e = &set->e[set->n++];
                snprintf(e->title, sizeof(e->title), "%s", a->title ? a->title : "");
                snprintf(e->desc, sizeof(e->desc), "%s", a->description ? a->description : "");
                e->points = a->points;
                e->unlocked = unlocked;
                set->pts += a->points;
                if (unlocked) { set->have++; set->pts_have += a->points; }
                char url[512];
                const char *name = NULL;
                if (rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url)) == RC_OK) {
                    name = strrchr(url, '/');
                    name = name ? name + 1 : url;
                }
                if (name && g_img_dir[0]) snprintf(e->icon, sizeof(e->icon), "%s/%s", g_img_dir, name);
            }
    rc_client_destroy_achievement_list(list);
    return set->n > 0;
}

int rt_ra_set(RtRaSet *set) { return set_from(g_client, set); }

void rt_ra_set_free(RtRaSet *set) {
    free(set->e);
    memset(set, 0, sizeof(*set));
}

/* a field of achievements.txt: tabs and line breaks would split it */
static void put_field(FILE *f, const char *s) {
    for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

static void write_summary(rc_client_t *client) {
    char path[600], tmp[620];
    const char *test = getenv("RT_TEST_RA_SUMMARY");
    if (test && *test) snprintf(path, sizeof(path), "%s", test);
    else {
        const char *settings = rt_gpu_opt.settings, *slash = settings ? strrchr(settings, '/') : NULL;
        if (g_spectator || !slash) return;
        snprintf(path, sizeof(path), "%.*s/achievements.txt", (int)(slash - settings), settings);
    }
    RtRaSet set;
    if (!set_from(client, &set)) return;
    const rc_client_game_t *g = rc_client_get_game_info(client);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "# RetroAchievements, written by the game for the game picker's list (tab-separated)\n");
        fprintf(f, "game\t%u\t", g ? g->id : 0);
        put_field(f, set.game);
        fprintf(f, "\t%d\t%d\t%u\t%u\n", set.have, set.n, set.pts_have, set.pts);
        for (int i = 0; i < set.n; i++) {
            const RtRaEntry *e = &set.e[i];
            fprintf(f, "a\t%d\t%u\t", e->unlocked, e->points);
            put_field(f, e->title);
            fputc('\t', f);
            put_field(f, e->desc);
            fputc('\t', f);
            put_field(f, e->icon);
            fputc('\n', f);
        }
        if (fclose(f) == 0) rename(tmp, path);
        else remove(tmp);
    }
    rt_ra_set_free(&set);
}

/* --- memory and events ------------------------------------------------------------- */
static uint32_t read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client) {
    (void)client;
    if (address >= 0x04000000u || num_bytes > 0x04000000u - address) return 0;
    memcpy(buffer, rt_ptr(0x08000000u + address), num_bytes);
    return num_bytes;
}

static void log_message(const char *message, const rc_client_t *client) {
    (void)client;
    printf("[ra] rcheevos: %s\n", message);
}

static void on_event(const rc_client_event_t *e, rc_client_t *client) {
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        printf("[ra] achievement%s: %s (%u points) - %s\n", g_spectator ? " (watching, not sent)" : "", e->achievement->title,
               e->achievement->points, e->achievement->description);
        char url[512], pts[32];
        if (rc_client_achievement_get_image_url(e->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url)) != RC_OK) url[0] = 0;
        if (!e->achievement->points && !strncmp(e->achievement->title, "Warning:", 8)) {  /* the server's notes, every session */
            if (strstr(e->achievement->title, "Unknown Emulator"))
                notice("RetroAchievements", NULL, "Modo normal (softcore)",
                       "Este programa todavía no está aprobado para el modo hardcore: los logros valen en modo normal.", url);
            else notice("RetroAchievements", NULL, e->achievement->title + (e->achievement->title[8] == ' ' ? 9 : 8), e->achievement->description, url);
            break;
        }
        snprintf(pts, sizeof(pts), "%u %s", e->achievement->points, e->achievement->points == 1 ? "punto" : "puntos");
        notice(g_spectator ? "Logro (prueba: no se envía)" : "Logro desbloqueado", pts, e->achievement->title,
               e->achievement->description, url);
        write_summary(client);
        break;
    }
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED: printf("[ra] leaderboard started: %s\n", e->leaderboard->title); break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED: printf("[ra] leaderboard failed: %s\n", e->leaderboard->title); break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        printf("[ra] leaderboard: %s = %s\n", e->leaderboard->title, e->leaderboard->tracker_value);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        printf("[ra] the set is complete\n");
        const rc_client_game_t *g = rc_client_get_game_info(client);
        char url[512];
        if (!g || rc_client_game_get_image_url(g, url, sizeof(url)) != RC_OK) url[0] = 0;
        notice("Juego completado", NULL, g ? g->title : "", "Ganaste todos los logros de este juego.", url);
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        printf("[ra] server error: %s %s\n", e->server_error->api ? e->server_error->api : "", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        printf("[ra] disconnected: unlocks wait to be sent\n");
        notice("RetroAchievements", NULL, "Sin conexión", "Los logros que ganes se envían cuando vuelva la conexión.", NULL);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        printf("[ra] reconnected\n");
        notice("RetroAchievements", NULL, "Conexión recuperada", "Los logros pendientes ya se enviaron.", NULL);
        break;
    default: break;
    }
}

static void on_load(int result, const char *error, rc_client_t *client, void *userdata) {
    (void)userdata;
    if (result != RC_OK) {
        printf("[ra] game not loaded: %s\n", error ? error : rc_error_str(result));
        if (result == RC_NO_GAME_LOADED)
            notice("RetroAchievements", NULL, "Este disco no tiene logros", "RetroAchievements reconoce el disco europeo de este juego.", NULL);
        else notice("RetroAchievements", NULL, "No se pudieron cargar los logros", error ? error : rc_error_str(result), NULL);
        return;
    }
    const rc_client_game_t *g = rc_client_get_game_info(client);
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(client, &s);
    printf("[ra] %s (game %u): %u of %u achievements unlocked%s\n", g->title, g->id, s.num_unlocked_achievements,
           s.num_core_achievements, g_spectator ? "; watching only, nothing is sent" : "");
    char url[512], text[160], pts[48];
    if (rc_client_game_get_image_url(g, url, sizeof(url)) != RC_OK) url[0] = 0;
    snprintf(text, sizeof(text), "%u de %u logros ganados.%s", s.num_unlocked_achievements, s.num_core_achievements,
             g_spectator ? " Prueba: no se envía nada." : "");
    snprintf(pts, sizeof(pts), "%u de %u puntos", s.points_unlocked, s.points_core);
    notice(g_spectator ? "RetroAchievements: solo mirando" : "RetroAchievements", pts, g->title, text, url);
    prefetch_icons(client);
    write_summary(client);
    if (g_spectator && getenv("RT_TEST_RA_TOAST")) {  /* tests: the set's first real achievement as if unlocked, shown only */
        rc_client_achievement_list_t *list = rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                               RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        const rc_client_achievement_t *a = NULL;
        for (uint32_t b = 0; list && !a && b < list->num_buckets; b++)
            for (uint32_t k = 0; !a && k < list->buckets[b].num_achievements; k++)
                if (list->buckets[b].achievements[k]->points) a = list->buckets[b].achievements[k];  /* not the server's notes */
        if (a) {
            char pts1[32];
            if (rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof(url)) != RC_OK) url[0] = 0;
            snprintf(pts1, sizeof(pts1), "%u %s", a->points, a->points == 1 ? "punto" : "puntos");
            printf("[ra] RT_TEST_RA_TOAST: showing \"%s\" as if unlocked (nothing is sent)\n", a->title);
            notice("Logro (prueba: no se envía)", pts1, a->title, a->description, url);
        }
        if (list) rc_client_destroy_achievement_list(list);
    }
}

static void on_login(int result, const char *error, rc_client_t *client, void *userdata) {
    (void)userdata;
    if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED) {
        printf("[ra] sign-in refused: %s (sign in again: Start in the game picker)\n", error && *error ? error : rc_error_str(result));
        notice("RetroAchievements", NULL, "No se pudo iniciar sesión",
               "Volvé a entrar desde el selector (Start). Esta partida sigue sin logros.", NULL);
        return;
    }
    if (result != RC_OK) {  /* no answer: offline, or the server down. Tried again every minute while playing */
        printf("[ra] no connection to RetroAchievements (%s): trying again in a minute\n", error && *error ? error : rc_error_str(result));
        if (!g_offline_told)
            notice("RetroAchievements", NULL, "Sin conexión",
                   "Por ahora esta partida no da logros. Se vuelve a intentar cada minuto.", NULL);
        g_offline_told = 1;
        g_login_retry_ms = mono_ms() + 60000;
        return;
    }
    g_login_retry_ms = 0;
    g_logged_in = 1;
    printf("[ra] signed in as %s\n", rc_client_get_user_info(client)->display_name);
    if (g_hash[0]) rc_client_begin_load_game(client, g_hash, on_load, NULL);
}

/* --- the runtime's side ------------------------------------------------------------ */
int rt_ra_option(int argc, char **argv, int *i) {
    if (!strcmp(argv[*i], "--retroachievements") && *i + 1 < argc) { g_ini = argv[++*i]; return 1; }
    if (!strcmp(argv[*i], "--ra-spectator")) { g_spectator = 1; return 1; }
    return 0;
}

static int read_ini(void) {
    FILE *f = fopen(g_ini, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "user=", 5)) snprintf(g_user, sizeof(g_user), "%s", line + 5);
        if (!strncmp(line, "token=", 6)) snprintf(g_token, sizeof(g_token), "%s", line + 6);
    }
    fclose(f);
    return g_user[0] && g_token[0];
}

static int disc_hash(void) {
    uint32_t n1 = 0, n2 = 0;
    uint8_t *sfo = rt_io_read_disc_file("disc0:/PSP_GAME/PARAM.SFO", &n1);
    uint8_t *eboot = sfo ? rt_io_read_disc_file("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &n2) : NULL;
    if (!sfo || !eboot) { free(sfo); free(eboot); return 0; }
    md5_state_t st;
    md5_byte_t d[16];
    md5_init(&st);
    md5_append(&st, sfo, (int)n1);
    md5_append(&st, eboot, (int)(n2 < 64u * 1024 * 1024 ? n2 : 64u * 1024 * 1024));
    md5_finish(&st, d);
    for (int k = 0; k < 16; k++) snprintf(g_hash + 2 * k, 3, "%02x", d[k]);
    free(sfo);
    free(eboot);
    return 1;
}

void rt_ra_start(int cheats) {
    if (!g_ini) return;
    if (!read_ini()) { printf("[ra] %s has no user and token: run tools/ra_login.py\n", g_ini); return; }
    if (!curl_load()) {
        printf("[ra] libcurl not found: playing without achievements\n");
        notice("RetroAchievements", NULL, "Sin logros en esta partida", "No se encontró libcurl, que hace falta para conectarse.", NULL);
        return;
    }
    const char *slash = strrchr(g_ini, '/');
    if (slash) snprintf(g_img_dir, sizeof(g_img_dir), "%.*s/ra_images", (int)(slash - g_ini), g_ini);
    else snprintf(g_img_dir, sizeof(g_img_dir), "ra_images");
    if (mkdir(g_img_dir, 0755) != 0 && errno != EEXIST) g_img_dir[0] = 0;
    if (!disc_hash()) { printf("[ra] couldn't read PARAM.SFO and EBOOT.BIN off the disc\n"); return; }
    if (cheats && !g_spectator) {
        g_spectator = 1;
        printf("[ra] --warp, --load-level, --god or --input: watching only, no unlock is sent\n");
    }
    g_client = rc_client_create(read_memory, server_call);
    if (!g_client) return;
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(g_client, on_event);
    rc_client_set_hardcore_enabled(g_client, 0);
    rc_client_set_spectator_mode_enabled(g_client, g_spectator);
    printf("[ra] disc hash %s; signing in as %s\n", g_hash, g_user);
    rc_client_begin_login_with_token(g_client, g_user, g_token, on_login, NULL);
}

void rt_ra_frame(uint64_t now_us) {
    static uint64_t frames;
    static char last[256];
    if (!g_client) return;
    if (g_login_retry_ms && mono_ms() >= g_login_retry_ms) {
        g_login_retry_ms = 0;
        rc_client_begin_login_with_token(g_client, g_user, g_token, on_login, NULL);
    }
    if (!g_next_frame || now_us > g_next_frame + 1000000) g_next_frame = now_us;  /* first frame, or a long stall */
    for (int n = 0; now_us >= g_next_frame && n < 8; n++) {
        rc_client_do_frame(g_client);
        g_next_frame += FRAME_US;
        if (++frames % 600 == 0 && rc_client_is_game_loaded(g_client)) {  /* every 10 s: the rich presence, when it changes */
            char rp[256];
            if (rc_client_get_rich_presence_message(g_client, rp, sizeof(rp)) && strcmp(rp, last)) {
                memcpy(last, rp, sizeof(last));
                printf("[ra] rich presence: %s\n", rp);
            }
        }
    }
}

void rt_ra_idle(void) {
    if (g_client) rc_client_idle(g_client);
}

void rt_ra_shutdown(void) {
    if (!g_client) return;
    for (int i = 0; i < 50; i++) {  /* up to 5 s for unlocks on their way */
        pthread_mutex_lock(&g_req_lock);
        int busy = g_requests;
        pthread_mutex_unlock(&g_req_lock);
        if (!busy) break;
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
}

#else
int rt_ra_set(RtRaSet *set) { memset(set, 0, sizeof(*set)); return 0; }
void rt_ra_set_free(RtRaSet *set) { (void)set; }
int rt_ra_option(int argc, char **argv, int *i) { (void)argc; (void)argv; (void)i; return 0; }
void rt_ra_start(int cheats) { (void)cheats; }
void rt_ra_frame(uint64_t now_us) { (void)now_us; }
void rt_ra_idle(void) {}
void rt_ra_shutdown(void) {}
#endif
