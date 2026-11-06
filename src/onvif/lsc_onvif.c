/*
 * lsc_onvif.c — démon ONVIF (device / media / PTZ) embarqué DANS la caméra LSC PTZ.
 *
 * Remplace le shim Python hôte (onvif-shim/onvif_shim.py) : même contrat SOAP
 * (porté verbatim), mais exécuté sur la caméra (ARM926EJ-S / uClibc 1.0.28),
 * backend = ptzd TCP 127.0.0.1:8091.
 *
 *   build croisé : make            (ARM, lié uClibc)
 *   test hôte    : make host && ./lsc_onvif_host --self-test
 *   run caméra   : /usr/sbin/lsc_onvif [-c /etc/config/lsc_onvif.conf]
 *
 * Options : --ip A.B.C.D  --port N  --ptzd-port N  --conf PATH
 *           --self-test  --foreground  --version
 *
 * Le démon est mono-thread, un seul client HTTP à la fois (Frigate est
 * séquentiel), boucle poll() 50 ms avec machine à états moteur non bloquante.
 *
 * Auteur : agent LSC, 2026-11-04 (plan item 9).
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>

#include "snapshot_jpeg.h"

/*
 * Build croisé glibc-headers -> uClibc : avec _GNU_SOURCE, glibc >= 2.38
 * redirige sscanf() vers __isoc23_sscanf() (absent d'uClibc 1.0.28).
 * On fournit l'alias, qui délègue à vsscanf().
 */
extern int lsc_vsscanf_(const char *s, const char *fmt, __gnuc_va_list ap)
    __asm__("vsscanf");
extern int __isoc23_sscanf(const char *s, const char *fmt, ...);
int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = lsc_vsscanf_(s, fmt, ap);
    va_end(ap);
    return r;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

#define APP_NAME  "lsc-onvif"
#define APP_VER   "1.0.0"

/* ------------------------------------------------------------------ XML NS */
#define NS_S12   "http://www.w3.org/2003/05/soap-envelope"
#define NS_S11   "http://schemas.xmlsoap.org/soap/envelope/"
#define NS_TT    "http://www.onvif.org/ver10/schema"
#define NS_TDS   "http://www.onvif.org/ver10/device/wsdl"
#define NS_TRT   "http://www.onvif.org/ver10/media/wsdl"
#define NS_TPTZ  "http://www.onvif.org/ver20/ptz/wsdl"
#define NS_TEV   "http://www.onvif.org/ver10/events/wsdl"
#define NS_WSA   "http://www.w3.org/2005/08/addressing"
#define NS_TER   "http://www.onvif.org/ver10/error"

#define SP_BASE    "http://www.onvif.org/ver10/tptz/PanTiltSpaces"
#define SP_ABS     SP_BASE "/AbsolutePanTiltPositionSpace"
#define SP_GEN_REL SP_BASE "/TranslationGenericSpace"
#define SP_FOV     SP_BASE "/TranslationSpaceFov"
#define SP_CTRL    SP_BASE "/ContinuousPanTiltVelocitySpace"

/* --------------------------------------------------------------- constantes */
#define CENTER        2048          /* position home (pas) sur les 2 axes */
#define GOTO_TOL      8
#define TILT_SIGN     (+1.0)        /* ONVIF tilt+ = camera qui MONTE (convention Frigate).
                                      * MESURE DECISIVE 2026-11-05 16:09 (image, pas de chiffres bruts) :
                                      * cam_2 cadre le sol+pied => ONVIF RelativeMove y=+0.2 => l'image
                                      * montre le SOL (tuiles) => tilt+ faisait DESCENDRE => signe inverse.
                                      * Frigate (autotrack.py l.1223) : tilt = (0.5 - centroid_y/h)*2
                                      * => objet en HAUT de l'image = tilt POSITIF = camera doit MONTER.
                                      * Ancien -1.0 = cause du runaway autotracking (cam_2 partait vers
                                      * le plafond quand la cible etait basse dans l'image). */
/* Espace FOV : fraction de l'unite generique (+-1 = demi-debattement) valant UN
 * champ complet. Mesure sur cam_3 le 2026-11-04 ~22:20-22:50 (deplacement d'image
 * pour un pas connu, position relue en boucle fermee) :
 *   pan  : x=1.0 deplace ~2.25 champs  => 1 champ = 0.444 unite
 *   tilt : y=1.0 deplace ~1.22 champs  => 1 champ = 0.820 unite
 * Coherent avec un PTZ 355 deg et une optique ~80 deg horizontal (4.5 champs sur +-1).
 * L'ancienne valeur unique 0.5 supposait ~2.0 champs/unite (ecart ~2x en tilt). */
#define FOV_SPAN_FRAC_X 0.444
#define FOV_SPAN_FRAC_Y 0.820
/* AXE TILT : le debattement tilt n'utilise que la MOITIE de la plage brute
 * (raw 0 = sol, raw 1024 = vue de niveau "level", raw 2048 = plafond ; mesure
 * 2026-11-05 16:0x-16:1x sur cam_2 image par image). CENTER=2048 est donc le
 * HAUT mecanique, pas le milieu. On recentre donc l'espace generique tilt :
 *   y = 0    -> raw 1024 (vue de niveau)
 *   y = +0.5 -> raw 2048 (plafond)
 *   y = -0.5 -> raw 0    (sol)
 * TILT_SPAN = 2048 unites brutes par 1.0 generique (=> +-0.5 utile, dans +-1). */
#define TILT_ZERO     1024
#define TILT_SPAN     2048.0

/* Certaines cameras ont l'axe tilt mecaniquement inverse (montage different) :
 * y>0 doit toujours faire MONTER l'image (convention ONVIF/Frigate), donc on
 * rend le signe reglable par camera via tilt_sign dans la conf. */
static double g_tilt_sign = TILT_SIGN;
#define CHUNK_STEPS   300
#define CHUNK_MS      400
#define DEADZONE      0.05
#define DEADMAN_MS    2500
#define GLIDE_MS      350
#define GOTO_STEP_MS  600
#define GOTO_MAX_ITER 600
#define TICK_MS       50
#define PING_MS       20000

#define MAX_BODY    (32 * 1024)
#define MAX_RESP    (24 * 1024)
#define MAX_HDRS    (8 * 1024)
#define MAX_REQ     (MAX_HDRS + MAX_BODY)

/* ------------------------------------------------------------------ globales */
static const char *g_conf_path = "/etc/config/lsc_onvif.conf";
static char g_ip[32]           = "127.0.0.1";
static char g_iface[16]        = "";
static int  g_iface_set         = 0;
static int  g_port             = 8080;
static int  g_ptzd_port        = 8091;
static char g_main_uri[160]    = "";
static char g_sub_uri[160]     = "";
static int  g_have_sub         = 0;
static char g_fwver[64]        = APP_VER;
static char g_serial[32]       = "LSC0000000000";
static volatile sig_atomic_t g_stop = 0;
static long long g_start_ms    = 0;

static void logmsg(const char *fmt, ...);

/* ------------------------------------------------------------------- string */
typedef struct { char *b; size_t cap, n; int oom; } sb;

static void sb_init(sb *s, char *buf, size_t cap)
{
    s->b = buf; s->cap = cap; s->n = 0; s->oom = 0;
    if (cap) buf[0] = 0;
}

static void sb_add(sb *s, const char *t)
{
    size_t l;
    if (s->oom) return;
    l = strlen(t);
    if (s->n + l + 1 > s->cap) { s->oom = 1; return; }
    memcpy(s->b + s->n, t, l + 1);
    s->n += l;
}

static void sb_fmt(sb *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (s->oom) return;
    if (s->n + 1 >= s->cap) { s->oom = 1; return; }
    va_start(ap, fmt);
    r = vsnprintf(s->b + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (r < 0) { s->oom = 1; return; }
    if ((size_t)r >= s->cap - s->n) { s->oom = 1; s->n = s->cap - 1; return; }
    s->n += (size_t)r;
}

static void sb_esc(sb *s, const char *t)
{
    for (; *t; t++) {
        switch (*t) {
        case '&':  sb_add(s, "&amp;");  break;
        case '<':  sb_add(s, "&lt;");   break;
        case '>':  sb_add(s, "&gt;");   break;
        case '"':  sb_add(s, "&quot;"); break;
        case '\'': sb_add(s, "&apos;"); break;
        default: {
            char one[2]; one[0] = *t; one[1] = 0; sb_add(s, one);
        }
        }
    }
}

/* ---------------------------------------------------------------------- log */
static int g_log_fd = -1;

static void log_open(void)
{
    struct stat st;
    if (g_log_fd >= 0) return;
    if (stat("/tmp/lsc_onvif.log", &st) == 0 && st.st_size > 262144)
        rename("/tmp/lsc_onvif.log", "/tmp/lsc_onvif.log.1");
    g_log_fd = open("/tmp/lsc_onvif.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static void logmsg(const char *fmt, ...)
{
    char line[400];
    char ts[80];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    va_list ap;
    int n;

    if (tm) snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
    else    snprintf(ts, sizeof ts, "?");

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n > sizeof line - 2) n = sizeof line - 2;
    if (n && line[n - 1] == '\n') line[n - 1] = 0;

    log_open();
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s %s\n", ts, line);
    else               dprintf(2, "%s %s\n", ts, line);
}

/* --------------------------------------------------------------------- temps */
static long long mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* -------------------------------------------------------------------- maths */
static double my_fabs(double v) { return v < 0 ? -v : v; }

static int my_round(double v)
{
    if (v >= 0) return (int)(v + 0.5);
    return -(int)(-v + 0.5);
}

/* "-0.1234" sans printf flottant (uClibc : on évite %f) */
static void fmt4(char *out, size_t cap, double v)
{
    int neg = v < 0;
    long ip, fp;
    if (neg) v = -v;
    if (v > 9999.0) v = 9999.0;
    ip = (long)v;
    fp = (long)((v - (double)ip) * 10000.0 + 0.5);
    if (fp >= 10000) { ip += 1; fp -= 10000; }
    snprintf(out, cap, "%s%ld.%04ld", neg ? "-" : "", ip, fp);
}

/* ------------------------------------------------------------------ fichiers */
static int iface_ip(const char *name, char *out, size_t cap);

/* Choisit l'interface qui porte réellement l'IPv4 (cam_3 = wlan0, d'autres = eth0). */
static void pick_iface(char *out, size_t cap)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    char tmp[32];
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!strcmp(e->d_name, "lo")) continue;
            if (strlen(e->d_name) >= sizeof g_iface) continue;
            if (iface_ip(e->d_name, tmp, sizeof tmp) == 0) {
                snprintf(out, cap, "%s", e->d_name);
                closedir(d);
                return;
            }
        }
        closedir(d);
    }
    snprintf(out, cap, "%s", "eth0");
}

static int read_first_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) return -1;
    if (!fgets(out, (int)cap, f)) { fclose(f); return -1; }
    fclose(f);
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = 0;
    return n ? 0 : -1;
}

static void load_identity(void)
{
    char mac[64] = {0};
    if (read_first_line("/usr/share/VERSION", g_fwver, sizeof g_fwver) != 0)
        snprintf(g_fwver, sizeof g_fwver, "%s", APP_VER);
    {
        char path[64];
        if (!g_iface[0]) pick_iface(g_iface, sizeof g_iface);
        snprintf(path, sizeof path, "/sys/class/net/%s/address", g_iface);
        if (read_first_line(path, mac, sizeof mac) != 0 && g_iface[0]) {
            char alt[16];
            pick_iface(alt, sizeof alt);
            if (strcmp(alt, g_iface)) {
                snprintf(g_iface, sizeof g_iface, "%s", alt);
                snprintf(path, sizeof path, "/sys/class/net/%s/address", g_iface);
                (void)read_first_line(path, mac, sizeof mac);
            }
        }
    }
    if (mac[0]) {
        size_t i, k = 0;
        for (i = 0; mac[i] && k + 2 < sizeof g_serial; i++)
            if (mac[i] != ':') g_serial[k++] = (char)toupper((unsigned char)mac[i]);
        g_serial[k] = 0;
    }
    if (!g_serial[0])
        snprintf(g_serial, sizeof g_serial, "LSC0000000000");
}

/* ------------------------------------------------------------------ réseau IP */
static int iface_ip(const char *name, char *out, size_t cap)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) { close(fd); return -1; }
    close(fd);
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        const char *s = inet_ntoa(sin->sin_addr);
        if (!s || !*s) return -1;
        snprintf(out, cap, "%s", s);
    }
    return 0;
}

/* --------------------------------------------------------------- RTSP probe */
/* DESCRIBE 127.0.0.1:554 <path> ; 1 si "200" dans la première ligne réponse. */
static int rtsp_probe(const char *path)
{
    int fd, ok = 0;
    struct sockaddr_in sa;
    char req[256], buf[256];
    ssize_t r;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(554);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    {
        struct timeval tv = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return 0; }
    snprintf(req, sizeof req,
             "DESCRIBE rtsp://127.0.0.1:554%s RTSP/1.0\r\n"
             "CSeq: 1\r\nUser-Agent: lsc-onvif\r\nAccept: application/sdp\r\n\r\n",
             path);
    if (send(fd, req, strlen(req), MSG_NOSIGNAL) > 0) {
        r = recv(fd, buf, sizeof buf - 1, 0);
        if (r > 0) {
            buf[r] = 0;
            if (strstr(buf, " 200 ")) ok = 1;
        }
    }
    close(fd);
    return ok;
}

static void probe_streams(void)
{
    snprintf(g_main_uri, sizeof g_main_uri, "rtsp://%s:554/main_ch", g_ip);
    g_have_sub = 0;
    if (rtsp_probe("/videoSub")) {
        snprintf(g_sub_uri, sizeof g_sub_uri, "rtsp://%s:554/videoSub", g_ip);
        g_have_sub = 1;
    } else if (rtsp_probe("/sub_ch")) {
        snprintf(g_sub_uri, sizeof g_sub_uri, "rtsp://%s:554/sub_ch", g_ip);
        g_have_sub = 1;
    }
    logmsg("streams: main=%s sub=%s", g_main_uri, g_have_sub ? g_sub_uri : "(absent)");
}

/* cam_3 est en WiFi : au lancement du démon (boucle lsc_boot, ~3 s après boot)
   wlan0 n'a pas encore d'adresse => l'IP auto était 127.0.0.1, ce qui faisait
   publier à ONVIF des URI rtsp://127.0.0.1:554/… (Frigate injoignable).
   On re-dérive donc l'IP à chaque requête tant qu'elle est fausse : un ioctl
   par requête, coût nul, et le client ONVIF est séquentiel (quelques req/min). */
static void refresh_net(void)
{
    char tmp[32], alt[16];

    if (!g_iface[0] || iface_ip(g_iface, tmp, sizeof tmp) != 0) {
        pick_iface(alt, sizeof alt);
        if (alt[0] && strcmp(alt, g_iface)) snprintf(g_iface, sizeof g_iface, "%s", alt);
        if (!g_iface[0] || iface_ip(g_iface, tmp, sizeof tmp) != 0) return;
    }
    if (strcmp(tmp, g_ip) == 0) return;
    snprintf(g_ip, sizeof g_ip, "%s", tmp);
    logmsg("net: ip corrigée -> %s (iface %s)", g_ip, g_iface);
    load_identity();
    probe_streams();
}

/* :554 met ~40 s à écouter après le boot : tant que /videoSub n'est pas
   confirmé, re-sonder au plus toutes les 30 s (2 DESCRIBE RTSP au pire). */
static unsigned long long g_last_subprobe = 0;

static void refresh_streams(void)
{
    unsigned long long now = mono_ms();
    if (g_have_sub) return;
    if (g_last_subprobe && now - g_last_subprobe < 30000ULL) return;
    g_last_subprobe = now;
    probe_streams();
}

/* ---------------------------------------------------------------------- ptzd */
static int g_ptz_fd = -1;

static void ptz_close(void)
{
    if (g_ptz_fd >= 0) {
        send(g_ptz_fd, "BYE\n", 4, MSG_NOSIGNAL);
        close(g_ptz_fd);
        g_ptz_fd = -1;
    }
}

static int ptz_connect(void)
{
    int fd;
    struct sockaddr_in sa;
    struct timeval tv = {8, 0};

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_ptzd_port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    g_ptz_fd = fd;
    return 0;
}

static int recv_line(int fd, char *out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0) return -1;
        if (ch == '\n') {
            if (i && out[i - 1] == '\r') i--;
            out[i] = 0;
            return 0;
        }
        out[i++] = ch;
    }
    out[i] = 0;
    return 0;
}

/* Envoie une commande ptzd, lit une ligne. 0 = OK, -1 = échec (socket fermée). */
static int ptz_req(const char *cmd, char *out, size_t cap)
{
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        char line[128];
        if (g_ptz_fd < 0) {
            int k;
            for (k = 0; k < 4 && g_ptz_fd < 0; k++) {
                if (ptz_connect() != 0) usleep(300000);
            }
            if (g_ptz_fd < 0) return -1;
        }
        snprintf(line, sizeof line, "%s\n", cmd);
        if (send(g_ptz_fd, line, strlen(line), MSG_NOSIGNAL) < 0 ||
            recv_line(g_ptz_fd, out, cap) != 0) {
            ptz_close();
            continue;
        }
        return 0;
    }
    return -1;
}

static int ptz_ping(void)
{
    char r[64];
    if (ptz_req("PING", r, sizeof r) != 0) return 0;
    return strncmp(r, "PONG", 4) == 0;
}

/* -> 0 si position lue. Format ptzd : "PAN pos=<n> state=<n> rc=<n> TILT pos=<n> ..." */
static int ptz_pos(int *pan, int *tilt)
{
    char r[192];
    const char *p, *t;
    if (ptz_req("POS", r, sizeof r) != 0) return -1;
    p = strstr(r, "PAN");
    if (p) p = strstr(p, "pos=");
    t = p ? strstr(r, "TILT") : NULL;
    if (t) t = strstr(t, "pos=");
    if (!p || !t) {
        logmsg("ptzd POS: réponse inattendue [%s]", r);
        return -1;
    }
    *pan = atoi(p + 4);
    *tilt = atoi(t + 4);
    return 0;
}

/* ptzd est mono-connexion et mono-thread : une requête POS lancée pile pendant
   une transition moteur peut revenir sans les champs pos=. On réessaie donc
   (3 x 200 ms) au lieu de faire échouer la requête ONVIF. */
static int ptz_pos3(int *pan, int *tilt)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (ptz_pos(pan, tilt) == 0) return 0;
        usleep(200000);
    }
    return -1;
}

static void ptz_move(int ax, int steps)
{
    char cmd[64], r[64];
    snprintf(cmd, sizeof cmd, "MOVE %d %d", ax, steps);
    if (ptz_req(cmd, r, sizeof r) != 0) logmsg("ptzd MOVE %d %d: unreachable", ax, steps);
    else if (strncmp(r, "OK", 2) != 0) logmsg("ptzd MOVE %d %d -> %s", ax, steps, r);
}

static void ptz_stop_ax(int ax)
{
    char cmd[32], r[64];
    snprintf(cmd, sizeof cmd, "STOP %d", ax);
    ptz_req(cmd, r, sizeof r);
}

static void ptz_stop_all(void)
{
    char r[64];
    ptz_req("STOP", r, sizeof r);
}

/* ------------------------------------------------------------- état moteur */
/* Transcription fidèle de ptzclient.MotorRunner (deadman 2.5 s, chunks 300,
 * re-arm 400 ms × max(0.3,|v|), glide 350 ms sur arrêt). */
static int       g_vel_on[2];
static double    g_vel[2];
static double    g_prev[2];
static long long g_glide_until[2];
static long long g_next_fire[2];
static long long g_last_cmd_ms;
static int       g_moving;    /* 1 si un axe bouge (pour GetStatus/MoveStatus) */

static void motor_set(double pan, double tilt)
{
    g_vel_on[0] = 1; g_vel[0] = pan;
    g_vel_on[1] = 1; g_vel[1] = tilt;
    g_last_cmd_ms = mono_ms();
}

static void motor_stop(int pan, int tilt)
{
    if (pan)  g_vel_on[0] = 0;
    if (tilt) g_vel_on[1] = 0;
    g_last_cmd_ms = mono_ms();
}

static void motor_halt(void)
{
    g_vel_on[0] = g_vel_on[1] = 0;
    g_last_cmd_ms = mono_ms();
}

/* --------------------------------------------------------------- goto (preset) */
static int       g_goto_active;
static int       g_goto_target[2];
static long long g_goto_next_ms;
static int       g_goto_iter;

static void goto_finish(void)
{
    g_goto_active = 0;
    ptz_stop_all();
}

static void goto_cancel(void)
{
    if (g_goto_active) {
        g_goto_active = 0;
        ptz_stop_all();
    }
}

static void goto_start(int pan, int tilt)
{
    g_goto_target[0] = pan;
    g_goto_target[1] = tilt;
    g_goto_iter = 0;
    g_goto_next_ms = mono_ms();
    g_goto_active = 1;
    motor_halt();
}

/* --------------------------------------------------------------- presets */
struct preset { char name[32]; int pan, tilt; };
static struct preset g_presets[16];
static int g_npresets;

static void preset_save(const char *name, int pan, int tilt);

/* Persistance des presets : sans elle, "level" disparait a chaque reboot et
 * Frigate retombe sur "home" (= position repliee) -> la camera regarde le
 * plafond. Fichier minuscule dans /etc/config (jffs2), ecriture atomique. */
#define PRESET_FILE "/etc/config/lsc_presets.txt"
static int g_presets_loading;

static void preset_store(void)
{
    char tmp[64];
    FILE *f;
    int i;
    if (g_presets_loading) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", PRESET_FILE);
    f = fopen(tmp, "w");
    if (!f) return;
    for (i = 0; i < g_npresets; i++)
        fprintf(f, "%s %d %d\n", g_presets[i].name, g_presets[i].pan, g_presets[i].tilt);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, PRESET_FILE) != 0) unlink(tmp);
}

static void preset_load(void)
{
    FILE *f = fopen(PRESET_FILE, "r");
    char line[128], *tok;
    int n = 0;
    if (!f) return;
    g_presets_loading = 1;
    while (fgets(line, sizeof line, f)) {
        char name[64];
        int pan, tilt;
        tok = strtok(line, " \t\r\n");
        if (!tok) continue;
        snprintf(name, sizeof name, "%s", tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok) continue;
        pan = atoi(tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok) continue;
        tilt = atoi(tok);
        if (g_npresets >= (int)(sizeof g_presets / sizeof g_presets[0])) break;
        preset_save(name, pan, tilt);
        n++;
    }
    g_presets_loading = 0;
    fclose(f);
    logmsg("presets: %d restaure(s) depuis %s", n, PRESET_FILE);
}

static struct preset *preset_find(const char *name)
{
    int i;
    for (i = 0; i < g_npresets; i++)
        if (!strcmp(g_presets[i].name, name)) return &g_presets[i];
    return NULL;
}

static void preset_save(const char *name, int pan, int tilt)
{
    struct preset *p = preset_find(name);
    if (!p) {
        if (g_npresets >= (int)(sizeof g_presets / sizeof g_presets[0])) return;
        size_t nl;
        p = &g_presets[g_npresets++];
        nl = strlen(name);
        if (nl >= sizeof p->name) nl = sizeof p->name - 1;
        memcpy(p->name, name, nl);
        p->name[nl] = 0;
    }
    p->pan = pan;
    p->tilt = tilt;
    preset_store();
}

static void preset_remove(const char *name)
{
    int i;
    for (i = 0; i < g_npresets; i++) {
        if (!strcmp(g_presets[i].name, name)) {
            int k;
            for (k = i + 1; k < g_npresets; k++) g_presets[k - 1] = g_presets[k];
            g_npresets--;
            preset_store();
            return;
        }
    }
}

/* ---------------------------------------------------------------------- XML */
static const char *find_elem(const char *s, const char *local)
{
    size_t ll = strlen(local);
    const char *p = s;
    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        const char *name, *colon;
        if (*q == '/') q++;
        name = q;
        while (*q && *q != '>' && *q != '/' && !isspace((unsigned char)*q)) q++;
        colon = memchr(name, ':', (size_t)(q - name));
        if (colon) name = colon + 1;
        if ((size_t)(q - name) == ll && strncmp(name, local, ll) == 0) return p;
        p++;
    }
    return NULL;
}

static int tag_attr(const char *tag, const char *name, char *out, size_t cap)
{
    const char *p = tag;
    size_t nl = strlen(name);
    while (*p && *p != '>') {
        const char *ns;
        size_t len;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '/' || *p == '>' || !*p) break;
        ns = p;
        while (*p && *p != '=' && *p != '>' && *p != ' ' && *p != '/' && *p != '\t') p++;
        len = (size_t)(p - ns);
        if (*p == '=') {
            char q;
            p++;
            q = *p;
            if (q == '"' || q == '\'') {
                const char *vs;
                size_t vlen;
                p++;
                vs = p;
                while (*p && *p != q) p++;
                if (len == nl && !strncmp(ns, name, nl)) {
                    vlen = (size_t)(p - vs);
                    if (vlen >= cap) vlen = cap - 1;
                    memcpy(out, vs, vlen);
                    out[vlen] = 0;
                    return 0;
                }
                if (*p == q) p++;
            } else {
                while (*p && *p != ' ' && *p != '>' && *p != '/') p++;
            }
        }
    }
    return -1;
}

static int elem_text(const char *start, char *out, size_t cap)
{
    const char *p = start;
    size_t n;
    while (*p && *p != '>') p++;
    if (!*p) return -1;
    p++;
    if (p > start && *(p - 2) == '/') { out[0] = 0; return 0; }
    n = 0;
    while (*p && *p != '<') {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\r' ||
                 out[n - 1] == '\n' || out[n - 1] == '\t')) out[--n] = 0;
    return 0;
}

/* texte d'un élément par nom, "" si absent */
static int req_text(const char *raw, const char *local, char *out, size_t cap)
{
    const char *p = find_elem(raw, local);
    out[0] = 0;
    if (!p) return -1;
    return elem_text(p, out, cap);
}

/* ------------------------------------------------------------------ requête */
struct reqinfo {
    int  so12;
    char action[256];
    char msgid[256];
    char elem[64];
    const char *raw;
};

static const char *soap_env(const struct reqinfo *ri)
{
    return ri->so12 ? NS_S12 : NS_S11;
}

static void soap_header(sb *s, const struct reqinfo *ri)
{
    sb_fmt(s, "<s:Header><wsa:Action>%s</wsa:Action><wsa:RelatesTo>%s</wsa:RelatesTo></s:Header>",
           ri->action, ri->msgid);
}

static void soap_begin(sb *s, const struct reqinfo *ri)
{
    sb_add(s, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    sb_fmt(s, "<s:Envelope xmlns:s=\"%s\" xmlns:tt=\"%s\" xmlns:tds=\"%s\""
              " xmlns:trt=\"%s\" xmlns:tptz=\"%s\" xmlns:tev=\"%s\""
              " xmlns:wsa=\"%s\" xmlns:ter=\"%s\">",
           soap_env(ri), NS_TT, NS_TDS, NS_TRT, NS_TPTZ, NS_TEV, NS_WSA, NS_TER);
    if (ri->action[0]) soap_header(s, ri);
    sb_add(s, "<s:Body>");
}

static void soap_end(sb *s) { sb_add(s, "</s:Body></s:Envelope>"); }

static void soap_fault(sb *s, const struct reqinfo *ri, const char *text)
{
    char esc[512];
    sb tmp;
    sb_init(&tmp, esc, sizeof esc);
    sb_esc(&tmp, text);
    sb_add(s, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    sb_fmt(s, "<s:Envelope xmlns:s=\"%s\" xmlns:tt=\"%s\">", soap_env(ri), NS_TT);
    sb_add(s, "<s:Body><s:Fault>");
    if (ri->so12) {
        sb_add(s, "<s:Code><s:Value>s:Receiver</s:Value></s:Code>"
                  "<s:Reason><s:Text xml:lang=\"en\">");
        sb_add(s, esc);
        sb_add(s, "</s:Text></s:Reason>");
    } else {
        sb_add(s, "<faultcode>s:Receiver</faultcode><faultstring>");
        sb_add(s, esc);
        sb_add(s, "</faultstring>");
    }
    sb_add(s, "</s:Fault></s:Body></s:Envelope>");
}

/* ================================================================== handlers */

static void xaddr(sb *s, const char *svc)
{
    sb_fmt(s, "http://%s:%d/onvif/%s", g_ip, g_port, svc);
}

/* --- noeuds communs --- */

static void ptz_config_node(sb *s)
{
    sb_add(s, "<tt:PTZConfiguration token=\"PTZConfig_1\">"
              "<tt:Name>PTZ</tt:Name>"
              "<tt:UseCount>1</tt:UseCount>"
              "<tt:NodeToken>PTZNode_1</tt:NodeToken>"
              "<tt:DefaultAbsolutePantTiltPositionSpace>" SP_ABS "</tt:DefaultAbsolutePantTiltPositionSpace>"
              "<tt:DefaultRelativePanTiltTranslationSpace>" SP_GEN_REL "</tt:DefaultRelativePanTiltTranslationSpace>"
              "<tt:DefaultContinuousPanTiltVelocitySpace>" SP_CTRL "</tt:DefaultContinuousPanTiltVelocitySpace>"
              "<tt:DefaultPTZTimeout>PT5S</tt:DefaultPTZTimeout>"
              "<tt:PanTiltLimits><tt:Range>"
              "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
              "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
              "<tt:Space>" SP_CTRL "</tt:Space>"
              "</tt:Range></tt:PanTiltLimits>"
              "</tt:PTZConfiguration>");
}

static void video_source_config(sb *s)
{
    sb_add(s, "<tt:VideoSourceConfiguration token=\"VideoSource_1\">"
              "<tt:Name>VideoSource</tt:Name>"
              "<tt:UseCount>1</tt:UseCount>"
              "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
              "<tt:Bounds height=\"0\" width=\"0\" maxY=\"0\" maxX=\"0\"/>"
              "</tt:VideoSourceConfiguration>");
}

static void encoder_config(sb *s, int width, int height, int fps, int bitrate)
{
    sb_fmt(s, "<tt:VideoEncoderConfiguration token=\"Encoder_1\">"
              "<tt:Name>Encoder</tt:Name>"
              "<tt:UseCount>1</tt:UseCount>"
              "<tt:Encoding>H264</tt:Encoding>"
              "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
              "<tt:Quality>4</tt:Quality>"
              "<tt:RateControl><tt:BitrateLimit>%d</tt:BitrateLimit>"
              "<tt:FrameRateLimit>%d</tt:FrameRateLimit></tt:RateControl>"
              "<tt:Multicast/>"
              "<tt:GOPLength>30</tt:GOPLength>"
              "<tt:Profile>Main</tt:Profile>"
              "</tt:VideoEncoderConfiguration>", width, height, bitrate, fps);
}

/* une entrée de profil */
static void profile_node(sb *s, int idx)
{
    if (idx == 0) {
        sb_add(s, "<trt:Profiles token=\"Profile_1\"><tt:Name>main</tt:Name>");
        video_source_config(s);
        encoder_config(s, 1920, 1080, 15, 4096);
        ptz_config_node(s);
        sb_add(s, "</trt:Profiles>");
    } else {
        sb_add(s, "<trt:Profiles token=\"Profile_2\"><tt:Name>sub</tt:Name>");
        video_source_config(s);
        encoder_config(s, 640, 480, 15, 1024);
        ptz_config_node(s);
        sb_add(s, "</trt:Profiles>");
    }
}

static const char *profile_uri(const char *tok)
{
    if (tok && (!strcmp(tok, "Profile_2") || !strcmp(tok, "sub")) && g_have_sub)
        return g_sub_uri;
    return g_main_uri;
}

static void simple_reply(sb *s, const struct reqinfo *ri, const char *tag,
                         const char *inner)
{
    soap_begin(s, ri);
    sb_add(s, tag);
    if (inner) sb_add(s, inner);
}

/* --- device --- */

static int h_get_services(sb *s, const struct reqinfo *ri)
{
    static const struct { const char *ns, *svc; } svcs[] = {
        { NS_TDS,  "device_service" },
        { NS_TRT,  "media_service"  },
        { NS_TPTZ, "ptz_service"    },
        { NS_TEV,  "events_service" },
    };
    unsigned i;
    soap_begin(s, ri);
    sb_add(s, "<tds:GetServicesResponse>");
    for (i = 0; i < sizeof svcs / sizeof svcs[0]; i++) {
        sb_add(s, "<tds:Service><tds:Namespace>");
        sb_add(s, svcs[i].ns);
        sb_add(s, "</tds:Namespace><tds:XAddr>");
        xaddr(s, svcs[i].svc);
        sb_add(s, "</tds:XAddr><tds:Version><tt:Major>1</tt:Major>"
                  "<tt:Minor>1</tt:Minor></tds:Version></tds:Service>");
    }
    sb_add(s, "</tds:GetServicesResponse>");
    return 0;
}

static int h_get_capabilities(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    /* Schema tds:Capabilities = sequence(Analytics?, Device?, Events?, Imaging?,
     * Media?, PTZ?, Extension?).  Pas de wrapper tt:Services : zeep range les
     * enfants hors séquence dans "_raw_elements" SANS erreur (symptôme : tous
     * les champs Capsabilities.* = None alors que le XML semble correct). */
    sb_add(s, "<tds:GetCapabilitiesResponse><tt:Capabilities>"
              "<tt:Device><tt:XAddr>");
    xaddr(s, "device_service");
    sb_add(s, "</tt:XAddr>"
              "<tt:Network><tt:IPFilter>false</tt:IPFilter>"
              "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
              "<tt:IPVersion6>false</tt:IPVersion6>"
              "<tt:DynDNS>false</tt:DynDNS></tt:Network></tt:Device>"
              "<tt:Events><tt:XAddr>");
    xaddr(s, "events_service");
    sb_add(s, "</tt:XAddr></tt:Events>"
              "<tt:Media><tt:XAddr>");
    xaddr(s, "media_service");
    sb_add(s, "</tt:XAddr></tt:Media>"
              "<tt:PTZ><tt:XAddr>");
    xaddr(s, "ptz_service");
    sb_add(s, "</tt:XAddr></tt:PTZ>"
              "</tt:Capabilities></tds:GetCapabilitiesResponse>");
    return 0;
}

static int h_get_system_date(sb *s, const struct reqinfo *ri)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    soap_begin(s, ri);
    sb_add(s, "<tds:GetSystemDateAndTimeResponse><tt:SystemDateAndTime>"
              "<tt:DateTimeType>UTC</tt:DateTimeType>"
              "<tt:DaylightSavings>false</tt:DaylightSavings>"
              "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>");
    if (tm) {
        sb_fmt(s, "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute>"
                  "<tt:Second>%d</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year>"
                  "<tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime>",
               tm->tm_hour, tm->tm_min, tm->tm_sec,
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    }
    sb_add(s, "</tt:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
    return 0;
}

static int h_get_device_information(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<tds:GetDeviceInformationResponse><tds:Manufacturer>LSC</tds:Manufacturer>"
              "<tds:Model>LSC PTZ</tds:Model><tds:FirmwareVersion>");
    sb_esc(s, g_fwver);
    sb_add(s, "</tds:FirmwareVersion><tds:SerialNumber>");
    sb_esc(s, g_serial);
    sb_add(s, "</tds:SerialNumber><tds:HardwareId>LSC-AK3918EV330</tds:HardwareId>"
              "</tds:GetDeviceInformationResponse>");
    return 0;
}

static int h_get_scopes(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<tds:GetScopesResponse><tds:Scopes>"
              "<tds:ScopeDef>Fixed</tds:ScopeDef>"
              "<tds:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tds:ScopeItem>"
              "<tds:ScopeItem>onvif://www.onvif.org/name/LSC-PTZ</tds:ScopeItem>"
              "<tds:ScopeItem>onvif://www.onvif.org/hardware/LSC-AK3918EV330</tds:ScopeItem>"
              "<tds:ScopeItem>onvif://www.onvif.org/type/network_video_transmitter</tds:ScopeItem>"
              "</tds:Scopes></tds:GetScopesResponse>");
    return 0;
}

static int h_get_network_interfaces(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<tds:GetNetworkInterfacesResponse>"
              "<tds:NetworkInterfaces token=\"eth0\">"
              "<tt:Name>eth0</tt:Name><tt:Enabled>true</tt:Enabled>"
              "<tt:Info/></tds:NetworkInterfaces>"
              "</tds:GetNetworkInterfacesResponse>");
    return 0;
}

/* --- media --- */

static int h_media_caps(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<trt:GetServiceCapabilitiesResponse>"
              "<trt:SnapshotUri>true</trt:SnapshotUri>"
              "<trt:Rotation>false</trt:Rotation>"
              "<trt:VideoSourceMode>false</trt:VideoSourceMode>"
              "<trt:OSD>false</trt:OSD>"
              "<trt:TemporaryOSDTextConfiguration>false</trt:TemporaryOSDTextConfiguration>"
              "<trt:TemporaryColorTvSettings>false</trt:TemporaryColorTvSettings>"
              "<trt:RTSPStreaming>true</trt:RTSPStreaming>"
              "</trt:GetServiceCapabilitiesResponse>");
    return 0;
}

static int h_get_profiles(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<trt:GetProfilesResponse>");
    profile_node(s, 0);
    if (g_have_sub) profile_node(s, 1);
    sb_add(s, "</trt:GetProfilesResponse>");
    return 0;
}

static int h_get_stream_uri(sb *s, const struct reqinfo *ri)
{
    char tok[64];
    const char *uri;
    req_text(ri->raw, "ProfileToken", tok, sizeof tok);
    uri = profile_uri(tok);
    soap_begin(s, ri);
    sb_add(s, "<trt:GetStreamUriResponse><tt:MediaUri><tt:Uri>");
    sb_esc(s, uri);
    sb_add(s, "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
              "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
              "<tt:Timeout>PT5S</tt:Timeout></tt:MediaUri></trt:GetStreamUriResponse>");
    return 0;
}

static int h_get_snapshot_uri(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<trt:GetSnapshotUriResponse><tt:MediaUri><tt:Uri>http://");
    sb_fmt(s, "%s:%d/snapshot.jpg", g_ip, g_port);
    sb_add(s, "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
              "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
              "<tt:Timeout>PT5S</tt:Timeout></tt:MediaUri></trt:GetSnapshotUriResponse>");
    return 0;
}

static int h_get_encoder_configs(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<trt:GetVideoEncoderConfigurationsResponse>");
    encoder_config(s, 1920, 1080, 15, 4096);
    if (g_have_sub) encoder_config(s, 640, 480, 15, 1024);
    sb_add(s, "</trt:GetVideoEncoderConfigurationsResponse>");
    return 0;
}

static int h_get_video_sources(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<trt:GetVideoSourcesResponse>"
              "<tt:VideoSources token=\"VideoSource_1\">"
              "<tt:Framerate>15</tt:Framerate>"
              "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
              "</tt:VideoSources></trt:GetVideoSourcesResponse>");
    return 0;
}

/* --- PTZ --- */

static int h_ptz_caps(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    /* MoveStatus/StatusPosition = attributs (xs:attribute du type tptz:Capabilities) :
     * nécessaires à l'autotracking Frigate. */
    sb_add(s, "<tptz:GetServiceCapabilitiesResponse>"
              "<tptz:Capabilities MoveStatus=\"true\" StatusPosition=\"true\"/>"
              "</tptz:GetServiceCapabilitiesResponse>");
    return 0;
}

/* REFERENCE TILT = position RAW du preset "level" (vue de veille choisie a
 * l'installation). y=0 correspond donc TOUJOURS a cette vue, meme si le
 * compteur de ptzd derive apres une saturation mecanique. Sans preset connu on
 * retombe sur TILT_ZERO (milieu de plage). */
static int g_tilt_ref = TILT_ZERO;

static void tilt_ref_note(int raw_tilt)
{
    if (raw_tilt >= 0 && raw_tilt <= 4095) g_tilt_ref = raw_tilt;
}

static int h_ptz_configurations(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<tptz:GetConfigurationsResponse>");
    ptz_config_node(s);
    sb_add(s, "</tptz:GetConfigurationsResponse>");
    return 0;
}

static int h_ptz_status(sb *s, const struct reqinfo *ri)
{
    int pan = CENTER, tilt = CENTER;
    char xs[32], ys[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    const char *move = "IDLE";

    if (ptz_pos(&pan, &tilt) != 0) { pan = CENTER; tilt = CENTER; }
    if (g_moving) move = "MOVING";

    fmt4(xs, sizeof xs, (double)(pan - CENTER) / (double)CENTER);
    fmt4(ys, sizeof ys, (g_tilt_sign * (double)(tilt - g_tilt_ref)) / TILT_SPAN);

    soap_begin(s, ri);
    sb_add(s, "<tptz:GetStatusResponse><tt:PTZStatus><tt:Position>");
    sb_fmt(s, "<tt:PanTilt x=\"%s\" y=\"%s\" space=\"" SP_ABS "\"/>", xs, ys);
    sb_add(s, "</tt:Position><tt:MoveStatus><tt:PanTilt>");
    sb_add(s, move);
    sb_add(s, "</tt:PanTilt><tt:Zoom>IDLE</tt:Zoom></tt:MoveStatus>");
    if (tm) {
        sb_fmt(s, "<tt:UtcTime>%04d-%02d-%02dT%02d:%02d:%02dZ</tt:UtcTime>",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    sb_add(s, "</tt:PTZStatus></tptz:GetStatusResponse>");
    return 0;
}

static void space2d(sb *s, const char *field, const char *uri,
                    const char *xmin, const char *xmax,
                    const char *ymin, const char *ymax)
{
    sb_fmt(s, "<tt:%s><tt:URI>%s</tt:URI>"
              "<tt:XRange><tt:Min>%s</tt:Min><tt:Max>%s</tt:Max></tt:XRange>"
              "<tt:YRange><tt:Min>%s</tt:Min><tt:Max>%s</tt:Max></tt:YRange>"
              "</tt:%s>", field, uri, xmin, xmax, ymin, ymax, field);
}

static int h_ptz_options(sb *s, const struct reqinfo *ri)
{
    soap_begin(s, ri);
    sb_add(s, "<tptz:GetConfigurationOptionsResponse>"
              "<tt:PTZConfigurationOptions><tt:Spaces>");
    /* ordre xs:sequence: Absolute, Relative(générique), Relative(FOV), Continuous */
    space2d(s, "AbsolutePanTiltPositionSpace", SP_ABS, "-1", "1", "-1", "1");
    space2d(s, "RelativePanTiltTranslationSpace", SP_GEN_REL, "-1", "1", "-1", "1");
    space2d(s, "RelativePanTiltTranslationSpace", SP_FOV,
            "-0.444", "0.444", "-0.820", "0.820");
    space2d(s, "ContinuousPanTiltVelocitySpace", SP_CTRL, "-1", "1", "-1", "1");
    sb_add(s, "</tt:Spaces><tt:PTZTimeout><tt:Min>PT1S</tt:Min>"
              "<tt:Max>PT30S</tt:Max></tt:PTZTimeout>"
              "</tt:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>");
    return 0;
}

/* attrs x/y d'un PanTilt situé après le premier élément "anchor" */
static int pantilt_xy(const char *raw, const char *anchor, double *x, double *y)
{
    const char *a = find_elem(raw, anchor);
    const char *pt;
    char v[32];
    *x = 0.0; *y = 0.0;
    if (!a) return -1;
    pt = find_elem(a, "PanTilt");
    if (!pt) return -1;
    if (tag_attr(pt, "x", v, sizeof v) == 0) *x = atof(v);
    if (tag_attr(pt, "y", v, sizeof v) == 0) *y = atof(v);
    return 0;
}

static int h_continuous_move(sb *s, const struct reqinfo *ri)
{
    double x = 0.0, y = 0.0;
    if (pantilt_xy(ri->raw, "Velocity", &x, &y) != 0)
        pantilt_xy(ri->raw, "Translation", &x, &y);
    goto_cancel();
    motor_set(x, g_tilt_sign * y);
    logmsg("ContinuousMove x=%.3f y=%.3f", x, y);
    simple_reply(s, ri, "<tptz:ContinuousMoveResponse/>", NULL);
    return 0;
}

static int h_relative_move(sb *s, const struct reqinfo *ri)
{
    double x = 0.0, y = 0.0;
    int cp, ct, tp, tt;
    if (pantilt_xy(ri->raw, "Translation", &x, &y) != 0) x = y = 0.0;
    if (ptz_pos3(&cp, &ct) != 0) {
        soap_fault(s, ri, "RelativeMove: no position from ptzd");
        return 1;
    }
    tp = cp + my_round(x * (double)CENTER);
    tt = ct + my_round(g_tilt_sign * y * TILT_SPAN);
    goto_cancel();
    goto_start(tp, tt);
    logmsg("RelativeMove x=%.3f y=%.3f -> target %d,%d", x, y, tp, tt);
    simple_reply(s, ri, "<tptz:RelativeMoveResponse/>", NULL);
    return 0;
}

static int h_stop(sb *s, const struct reqinfo *ri)
{
    const char *el = find_elem(ri->raw, "PanTilt");
    int pan = 1, tilt = 1;
    char v[16];
    if (el && elem_text(el, v, sizeof v) == 0) {
        pan = tilt = (strcasecmp(v, "true") == 0);
    }
    motor_stop(pan, tilt);
    goto_cancel();
    logmsg("Stop pan=%d tilt=%d", pan, tilt);
    simple_reply(s, ri, "<tptz:StopResponse/>", NULL);
    return 0;
}

static int h_get_presets(sb *s, const struct reqinfo *ri)
{
    int i;
    soap_begin(s, ri);
    sb_add(s, "<tptz:GetPresetsResponse>");
    if (!preset_find("home"))
        sb_add(s, "<tt:Preset token=\"preset_home\"><tt:Name>home</tt:Name></tt:Preset>");
    for (i = 0; i < g_npresets; i++) {
        sb_add(s, "<tt:Preset token=\"preset_");
        sb_esc(s, g_presets[i].name);
        sb_add(s, "\"><tt:Name>");
        sb_esc(s, g_presets[i].name);
        sb_add(s, "</tt:Name></tt:Preset>");
    }
    sb_add(s, "</tptz:GetPresetsResponse>");
    return 0;
}

static void goto_preset_by_name(const char *name)
{
    struct preset *p = preset_find(name);
    if (p) goto_start(p->pan, p->tilt);
    else    goto_start(CENTER, CENTER);
}

static int h_goto_preset(sb *s, const struct reqinfo *ri)
{
    char tok[64], name[64];
    const char *p;
    req_text(ri->raw, "PresetToken", tok, sizeof tok);
    if (!tok[0]) snprintf(tok, sizeof tok, "home");
    p = tok;
    if (!strncmp(p, "preset_", 7)) p += 7;
    snprintf(name, sizeof name, "%s", p);
    goto_cancel();
    goto_preset_by_name(name);
    {
        struct preset *pp = preset_find(name);
        if (pp && !strcmp(name, "level")) tilt_ref_note(pp->tilt);
    }
    logmsg("GotoPreset %s", name);
    simple_reply(s, ri, "<tptz:GotoPresetResponse/>", NULL);
    return 0;
}

static int h_goto_home(sb *s, const struct reqinfo *ri)
{
    goto_cancel();
    goto_preset_by_name("home");
    logmsg("GotoHomePosition");
    simple_reply(s, ri, "<tptz:GotoHomePositionResponse/>", NULL);
    return 0;
}

static int h_set_preset(sb *s, const struct reqinfo *ri)
{
    char name[64], tok[64];
    int pan = CENTER, tilt = CENTER;
    req_text(ri->raw, "PresetName", name, sizeof name);
    if (!name[0]) {
        req_text(ri->raw, "PresetToken", tok, sizeof tok);
        if (tok[0]) {
            const char *q = tok;
            if (!strncmp(q, "preset_", 7)) q += 7;
            snprintf(name, sizeof name, "%s", q);
        }
    }
    if (!name[0]) snprintf(name, sizeof name, "preset");
    if (ptz_pos(&pan, &tilt) != 0) { pan = CENTER; tilt = CENTER; }
    preset_save(name, pan, tilt);
    if (!strcmp(name, "level")) tilt_ref_note(tilt);
    logmsg("SetPreset %s = %d,%d%s", name, pan, tilt,
           !strcmp(name, "level") ? " (reference tilt)" : "");
    soap_begin(s, ri);
    sb_add(s, "<tptz:SetPresetResponse><tptz:PresetToken>preset_");
    sb_esc(s, name);
    sb_add(s, "</tptz:PresetToken></tptz:SetPresetResponse>");
    return 0;
}

static int h_remove_preset(sb *s, const struct reqinfo *ri)
{
    char tok[64];
    const char *p;
    req_text(ri->raw, "PresetToken", tok, sizeof tok);
    p = tok;
    if (!strncmp(p, "preset_", 7)) p += 7;
    if (*p) preset_remove(p);
    logmsg("RemovePreset %s", p);
    simple_reply(s, ri, "<tptz:RemovePresetResponse/>", NULL);
    return 0;
}

/* ================================================================= dispatch */

static int is_onvif(const char *s, const char *ns)
{
    return strstr(s, ns) != NULL;
}

/* renvoie 0 = réponse écrite ; -1 = handler a écrit un Fault ; -2 = inconnu */
static int soap_dispatch(const struct reqinfo *ri, sb *s)
{
    const char *e = ri->elem;

    if (!strcmp(e, "GetServiceCapabilities")) {
        if (is_onvif(ri->raw, "ver20/ptz")) {
            h_ptz_caps(s, ri);
        } else {
            h_media_caps(s, ri);
        }
        soap_end(s);
        return 0;
    }
    if (!strcmp(e, "GetServices"))            { h_get_services(s, ri); }
    else if (!strcmp(e, "GetCapabilities"))   { h_get_capabilities(s, ri); }
    else if (!strcmp(e, "GetSystemDateAndTime")) { h_get_system_date(s, ri); }
    else if (!strcmp(e, "GetDeviceInformation")) { h_get_device_information(s, ri); }
    else if (!strcmp(e, "GetScopes"))         { h_get_scopes(s, ri); }
    else if (!strcmp(e, "GetNetworkInterfaces")) { h_get_network_interfaces(s, ri); }
    else if (!strcmp(e, "GetProfiles"))       { h_get_profiles(s, ri); }
    else if (!strcmp(e, "GetStreamUri"))      { h_get_stream_uri(s, ri); }
    else if (!strcmp(e, "GetSnapshotUri"))    { h_get_snapshot_uri(s, ri); }
    else if (!strcmp(e, "GetVideoEncoderConfigurations")) { h_get_encoder_configs(s, ri); }
    else if (!strcmp(e, "GetVideoSources"))   { h_get_video_sources(s, ri); }
    else if (!strcmp(e, "GetConfigurations")) { h_ptz_configurations(s, ri); }
    else if (!strcmp(e, "GetStatus"))         { h_ptz_status(s, ri); }
    else if (!strcmp(e, "GetConfigurationOptions")) { h_ptz_options(s, ri); }
    else if (!strcmp(e, "ContinuousMove"))    { h_continuous_move(s, ri); }
    else if (!strcmp(e, "RelativeMove"))      { return h_relative_move(s, ri); }
    else if (!strcmp(e, "Stop"))              { h_stop(s, ri); }
    else if (!strcmp(e, "GetPresets"))        { h_get_presets(s, ri); }
    else if (!strcmp(e, "GotoPreset"))        { h_goto_preset(s, ri); }
    else if (!strcmp(e, "SetPreset"))         { h_set_preset(s, ri); }
    else if (!strcmp(e, "RemovePreset"))      { h_remove_preset(s, ri); }
    else if (!strcmp(e, "GotoHomePosition"))  { h_goto_home(s, ri); }
    else return -2;

    soap_end(s);
    return 0;
}

/* ------------------------------------------------------- analyse de requête */
static int parse_request(const char *raw, struct reqinfo *ri)
{
    const char *b;
    const char *p;
    size_t n = 0;

    memset(ri, 0, sizeof *ri);
    ri->raw = raw;
    /* enveloppe : SOAP 1.2 ? */
    ri->so12 = strstr(raw, "2003/05/soap-envelope") != NULL;

    /* wsa:Action / wsa:MessageID (facultatifs) */
    req_text(raw, "Action", ri->action, sizeof ri->action);
    req_text(raw, "MessageID", ri->msgid, sizeof ri->msgid);
    if (ri->action[0]) {
        char *r = strstr(ri->action, "Request");
        if (r && strlen(r) == 7) strcpy(r, "Response");
    }

    /* premier élément enfant de s:Body */
    b = strstr(raw, "Body>");
    if (!b) return -1;
    p = b + 5;
    while (*p && *p != '<') p++;
    if (!*p) return -1;
    p++;
    if (*p == '/') return -1;
    while (*p && *p != ':' && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) {
        if (n + 1 < sizeof ri->elem) ri->elem[n++] = *p;
        p++;
    }
    if (*p == ':') {
        n = 0;
        p++;
        while (*p && *p != '>' && *p != '/' && !isspace((unsigned char)*p)) {
            if (n + 1 < sizeof ri->elem) ri->elem[n++] = *p;
            p++;
        }
    }
    ri->elem[n] = 0;
    return ri->elem[0] ? 0 : -1;
}

/* ====================================================================== HTTP */
static int send_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void http_reply(int fd, int code, const char *ctype,
                       const void *body, size_t len)
{
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                      "Content-Length: %u\r\nConnection: close\r\n"
                      "Server: " APP_NAME "/" APP_VER "\r\n\r\n",
                      code,
                      code == 200 ? "OK" : (code == 503 ? "Service Unavailable" : "Error"),
                      ctype, (unsigned)len);
    if (hl > 0) send_all(fd, hdr, (size_t)hl);
    if (len) send_all(fd, body, len);
}

static int hdr_value(const char *hdrs, size_t hlen, const char *name,
                     char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *line = hdrs;
    while (line < hdrs + hlen) {
        const char *eol = strstr(line, "\r\n");
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
        if (linelen > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            size_t vlen;
            while (*v == ' ' || *v == '\t') v++;
            vlen = linelen - (size_t)(v - line);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return 0;
        }
        if (!eol) break;
        line = eol + 2;
    }
    return -1;
}

/* ------------------------------------------- BAS-4 : auth HTTP Basic admin/admin */
#define AUTH_USER "admin"
#define AUTH_PASS "admin"

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* decode base64 ; renvoie la longueur decodee, ou -1 */
static int b64_decode(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    int acc = 0, bits = 0;
    while (*in && *in != '=') {
        int v = b64_val((unsigned char)*in++);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= cap) return -1;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = 0;
    return (int)n;
}

/* 0 = accepte ; -1 = identifiants faux. Header absent => tolere (compat clients). */
static int auth_check(const char *req, size_t hlen)
{
    char h[256], dec[256];
    const char *p;
    if (hdr_value(req, hlen, "Authorization", h, sizeof h) != 0) return 0;
    p = h;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "Basic", 5) != 0) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (b64_decode(p, dec, sizeof dec) < 0) return -1;
    return strcmp(dec, AUTH_USER ":" AUTH_PASS) == 0 ? 0 : -1;
}

static void http_401(int fd)
{
    static const char body[] = "Unauthorized\n";
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 401 Unauthorized\r\n"
                      "WWW-Authenticate: Basic realm=\"" APP_NAME "\"\r\n"
                      "Content-Type: text/plain\r\nContent-Length: %u\r\n"
                      "Connection: close\r\n\r\n", (unsigned)(sizeof body - 1));
    if (hl > 0) send_all(fd, hdr, (size_t)hl);
    send_all(fd, body, sizeof body - 1);
}

static void build_health(sb *s)
{
    int i;
    int ok = ptz_ping();
    sb_add(s, "{\"ok\":");
    sb_add(s, ok ? "true" : "false");
    sb_add(s, ",\"service\":\"" APP_NAME "\"");
    sb_fmt(s, ",\"cam\":\"%s\",\"port\":%d", g_ip, g_port);
    sb_add(s, ",\"onvif\":\"on-camera\",\"version\":\"" APP_VER "\"");
    sb_add(s, ",\"ptzd\":");
    sb_add(s, ok ? "\"PONG\"" : "\"DOWN\"");
    sb_add(s, ",\"fov_span_frac\":0.444,\"fov_span_frac_y\":0.820");
    sb_add(s, ",\"profiles\":[");
    sb_add(s, "\"Profile_1\"");
    if (g_have_sub) sb_add(s, ",\"Profile_2\"");
    sb_add(s, "],\"presets\":[");
    if (!preset_find("home")) sb_add(s, "\"home\"");
    for (i = 0; i < g_npresets; i++) {
        if (i || !preset_find("home")) sb_add(s, ",");
        sb_add(s, "\"");
        sb_esc(s, g_presets[i].name);
        sb_add(s, "\"");
    }
    sb_add(s, "],\"uptime_s\":");
    sb_fmt(s, "%lld", (mono_ms() - g_start_ms) / 1000);
    sb_add(s, ",\"stream\":\"");
    sb_esc(s, g_main_uri);
    sb_add(s, "\"}");
}

static void handle_conn(int fd)
{
    static char req[MAX_REQ + 1];
    static char body[MAX_BODY + 1];
    static char resp[MAX_RESP];
    struct timeval tv = {5, 0};
    size_t len = 0, hlen = 0, need = 0, got = 0;
    char method[16], path[512], tmp[64];
    char *hdr_end = NULL;
    sb s;
    int code = 200, rc;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    while (len < MAX_HDRS) {
        ssize_t r = recv(fd, req + len, MAX_HDRS - len, 0);
        if (r <= 0) return;
        len += (size_t)r;
        req[len] = 0;
        if ((hdr_end = strstr(req, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) return;
    hlen = (size_t)(hdr_end - req) + 4;
    method[0] = path[0] = 0;
    if (sscanf(req, "%15s %511s", method, path) < 1) return;
    {
        char *q = strchr(path, '?');
        if (q) *q = 0;
    }

    /* BAS-4 : identifiants verifies s'ils sont fournis (admin/admin). */
    if (auth_check(req, hlen) != 0) {
        logmsg("401 %s %s", method, path);
        http_401(fd);
        return;
    }

    refresh_net();
    refresh_streams();

    /* corps */
    need = 0;
    if (hdr_value(req, hlen, "Content-Length", tmp, sizeof tmp) == 0)
        need = (size_t)atol(tmp);
    if (need > MAX_BODY) need = MAX_BODY;
    if (len > hlen) {
        size_t avail = len - hlen;
        if (avail > need) avail = need;
        memcpy(body, req + hlen, avail);
        got = avail;
    }
    while (got < need) {
        ssize_t r = recv(fd, body + got, need - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    body[got] = 0;

    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
        if (!strcmp(path, "/healthz")) {
            sb_init(&s, resp, sizeof resp);
            build_health(&s);
            if (s.oom) { http_reply(fd, 500, "text/plain", "overflow", 8); return; }
            code = strstr(resp, "\"ok\":true") ? 200 : 503;
            http_reply(fd, code, "application/json", resp, s.n);
            return;
        }
        if (!strcmp(path, "/snapshot.jpg") || !strcmp(path, "/snapshot")) {
            http_reply(fd, 200, "image/jpeg", SNAPSHOT_JPEG, SNAPSHOT_JPEG_LEN);
            return;
        }
        if (!strcmp(path, "/") || !strcmp(path, "/status")) {
            char txt[200];
            int n = snprintf(txt, sizeof txt,
                             APP_NAME " " APP_VER " cam=%s port=%d ptzd=%s\n",
                             g_ip, g_port, "127.0.0.1");
            http_reply(fd, 200, "text/plain", txt, (size_t)(n > 0 ? n : 0));
            return;
        }
        http_reply(fd, 404, "text/plain", "not found\n", 10);
        return;
    }
    if (strcmp(method, "POST")) {
        http_reply(fd, 405, "text/plain", "method not allowed\n", 19);
        return;
    }

    {
        struct reqinfo ri;
        sb_init(&s, resp, sizeof resp);
        if (parse_request(body, &ri) != 0) {
            soap_fault(&s, &ri, "unparseable SOAP");
        } else {
            rc = soap_dispatch(&ri, &s);
            if (rc == -2) {
                sb_init(&s, resp, sizeof resp);
                soap_fault(&s, &ri, "not implemented");
                logmsg("unhandled: %s", ri.elem);
            } else if (rc == -1) {
                code = 500;
            } else {
                logmsg("%s %s -> ok (%u o)", method, ri.elem, (unsigned)s.n);
            }
        }
        if (s.oom) { http_reply(fd, 500, "text/plain", "overflow\n", 9); return; }
        http_reply(fd, code, "application/soap+xml; charset=utf-8", resp, s.n);
    }
}

/* ================================================================ self-test */
static int selftest(void)
{
    static const char *body_ptz_move =
        "<tptz:ContinuousMove xmlns:tptz=\"" NS_TPTZ "\" xmlns:tt=\"" NS_TT "\">"
        "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
        "<tptz:Velocity><tt:PanTilt x=\"0.5\" y=\"-0.25\"/></tptz:Velocity>"
        "</tptz:ContinuousMove>";
    static const char *body_ptz_rel =
        "<tptz:RelativeMove xmlns:tptz=\"" NS_TPTZ "\" xmlns:tt=\"" NS_TT "\">"
        "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
        "<tptz:Translation><tt:PanTilt x=\"0.1\" y=\"0\"/></tptz:Translation>"
        "</tptz:RelativeMove>";
    static const char *body_ptz_stop =
        "<tptz:Stop xmlns:tptz=\"" NS_TPTZ "\" xmlns:tt=\"" NS_TT "\">"
        "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
        "<tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>false</tptz:Zoom></tptz:Stop>";
    static const char *body_ptz_goto =
        "<tptz:GotoPreset xmlns:tptz=\"" NS_TPTZ "\">"
        "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
        "<tptz:PresetToken>preset_home</tptz:PresetToken></tptz:GotoPreset>";
    static const char *body_setpreset =
        "<tptz:SetPreset xmlns:tptz=\"" NS_TPTZ "\">"
        "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
        "<tptz:PresetName>bureau</tptz:PresetName></tptz:SetPreset>";

    struct test { const char *name; const char *ns; const char *body; };
    static const struct test tests[] = {
        { "GetServices",            NS_TDS,  "<tds:GetServices xmlns:tds=\"" NS_TDS "\"/>" },
        { "GetCapabilities",        NS_TDS,  "<tds:GetCapabilities xmlns:tds=\"" NS_TDS "\"/>" },
        { "GetSystemDateAndTime",   NS_TDS,  "<tds:GetSystemDateAndTime xmlns:tds=\"" NS_TDS "\"/>" },
        { "GetDeviceInformation",   NS_TDS,  "<tds:GetDeviceInformation xmlns:tds=\"" NS_TDS "\"/>" },
        { "GetScopes",              NS_TDS,  "<tds:GetScopes xmlns:tds=\"" NS_TDS "\"/>" },
        { "GetNetworkInterfaces",   NS_TDS,  "<tds:GetNetworkInterfaces xmlns:tds=\"" NS_TDS "\"/>" },
        { "media.GetServiceCapabilities", NS_TRT,
          "<trt:GetServiceCapabilities xmlns:trt=\"" NS_TRT "\"/>" },
        { "GetProfiles",            NS_TRT,  "<trt:GetProfiles xmlns:trt=\"" NS_TRT "\"/>" },
        { "GetStreamUri",           NS_TRT,
          "<trt:GetStreamUri xmlns:trt=\"" NS_TRT "\"><trt:ProfileToken>Profile_1</trt:ProfileToken></trt:GetStreamUri>" },
        { "GetSnapshotUri",         NS_TRT,
          "<trt:GetSnapshotUri xmlns:trt=\"" NS_TRT "\"><trt:ProfileToken>Profile_1</trt:ProfileToken></trt:GetSnapshotUri>" },
        { "GetVideoEncoderConfigurations", NS_TRT,
          "<trt:GetVideoEncoderConfigurations xmlns:trt=\"" NS_TRT "\"/>" },
        { "GetVideoSources",        NS_TRT,
          "<trt:GetVideoSources xmlns:trt=\"" NS_TRT "\"/>" },
        { "ptz.GetServiceCapabilities", NS_TPTZ,
          "<tptz:GetServiceCapabilities xmlns:tptz=\"" NS_TPTZ "\"/>" },
        { "GetConfigurations",      NS_TPTZ,
          "<tptz:GetConfigurations xmlns:tptz=\"" NS_TPTZ "\"/>" },
        { "GetStatus",              NS_TPTZ,
          "<tptz:GetStatus xmlns:tptz=\"" NS_TPTZ "\"><tptz:ProfileToken>Profile_1</tptz:ProfileToken></tptz:GetStatus>" },
        { "GetConfigurationOptions", NS_TPTZ,
          "<tptz:GetConfigurationOptions xmlns:tptz=\"" NS_TPTZ "\"><tptz:ProfileToken>Profile_1</tptz:ProfileToken></tptz:GetConfigurationOptions>" },
        { "ContinuousMove",         NS_TPTZ, NULL },
        { "RelativeMove",           NS_TPTZ, NULL },
        { "Stop",                   NS_TPTZ, NULL },
        { "GotoPreset",             NS_TPTZ, NULL },
        { "SetPreset",              NS_TPTZ, NULL },
        { "GetPresets",             NS_TPTZ,
          "<tptz:GetPresets xmlns:tptz=\"" NS_TPTZ "\"><tptz:ProfileToken>Profile_1</tptz:ProfileToken></tptz:GetPresets>" },
        { "GotoHomePosition",       NS_TPTZ,
          "<tptz:GotoHomePosition xmlns:tptz=\"" NS_TPTZ "\"><tptz:ProfileToken>Profile_1</tptz:ProfileToken></tptz:GotoHomePosition>" },
        { "RemovePreset",           NS_TPTZ,
          "<tptz:RemovePreset xmlns:tptz=\"" NS_TPTZ "\"><tptz:ProfileToken>Profile_1</tptz:ProfileToken><tptz:PresetToken>preset_bureau</tptz:PresetToken></tptz:RemovePreset>" },
        { "Bogus",                  NS_TPTZ,
          "<tptz:GetNothing xmlns:tptz=\"" NS_TPTZ "\"/>" },
    };
    static char reqbuf[8192];
    static char out[MAX_RESP];
    unsigned i;
    int fails = 0;

    /* état minimal pour que les réponses dépendant de l'identité soient réalistes */
    snprintf(g_ip, sizeof g_ip, "192.168.1.50");   /* self-test only */
    snprintf(g_main_uri, sizeof g_main_uri, "rtsp://%s:554/main_ch", g_ip);
    g_have_sub = 0;

    for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const struct test *t = &tests[i];
        struct reqinfo ri;
        const char *body = t->body;
        int rc;
        sb s;

        if (!body) {
            if (!strcmp(t->name, "ContinuousMove"))      body = body_ptz_move;
            else if (!strcmp(t->name, "RelativeMove"))   body = body_ptz_rel;
            else if (!strcmp(t->name, "Stop"))           body = body_ptz_stop;
            else if (!strcmp(t->name, "GotoPreset"))     body = body_ptz_goto;
            else if (!strcmp(t->name, "SetPreset"))      body = body_setpreset;
        }
        snprintf(reqbuf, sizeof reqbuf,
                 "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"" NS_S12 "\""
                 " xmlns:wsa=\"" NS_WSA "\"><s:Header>"
                 "<wsa:Action>http://test/%s</wsa:Action>"
                 "<wsa:MessageID>urn:uuid:00000000-0000-0000-0000-000000000001</wsa:MessageID>"
                 "</s:Header><s:Body>%s</s:Body></s:Envelope>", t->name, body);

        sb_init(&s, out, sizeof out);
        if (parse_request(reqbuf, &ri) != 0) {
            printf("=== %s PARSE_FAIL\n", t->name);
            fails++;
            continue;
        }
        rc = soap_dispatch(&ri, &s);
        if (rc == -2) {
            sb_init(&s, out, sizeof out);
            soap_fault(&s, &ri, "not implemented");
        }
        printf("=== %s rc=%d len=%u\n%s\n", t->name, rc, (unsigned)s.n, out);
        if (s.oom) { printf("=== %s OVERFLOW\n", t->name); fails++; }
    }
    printf("=== SUMMARY tests=%u fails=%d\n",
           (unsigned)(sizeof tests / sizeof tests[0]), fails);
    return fails ? 1 : 0;
}

/* ===================================================================== boucle */
static void tick(void)
{
    long long now = mono_ms();
    int ax;

    /* deadman */
    if (now - g_last_cmd_ms > DEADMAN_MS) {
        if (g_vel_on[0] || g_vel_on[1]) {
            g_vel_on[0] = g_vel_on[1] = 0;
            g_last_cmd_ms = now;
        }
    }

    /* goto (preset / relative) */
    if (g_goto_active) {
        if (now >= g_goto_next_ms) {
            if (++g_goto_iter > GOTO_MAX_ITER) {
                goto_finish();
            } else {
                int cp, ct;
                if (ptz_pos3(&cp, &ct) != 0) {
                    g_goto_next_ms = now + 300;
                } else {
                    int dp = g_goto_target[0] - cp;
                    int dt = g_goto_target[1] - ct;
                    if (my_fabs((double)dp) <= GOTO_TOL && my_fabs((double)dt) <= GOTO_TOL) {
                        goto_finish();
                    } else {
                        if (my_fabs((double)dp) > GOTO_TOL)
                            ptz_move(0, dp > 300 ? 300 : (dp < -300 ? -300 : dp));
                        if (my_fabs((double)dt) > GOTO_TOL)
                            ptz_move(1, dt > 300 ? 300 : (dt < -300 ? -300 : dt));
                        g_goto_next_ms = now + GOTO_STEP_MS;
                    }
                }
            }
        }
    }

    /* vitesse -> chunks moteur */
    g_moving = 0;
    for (ax = 0; ax < 2; ax++) {
        if (g_vel_on[ax] && my_fabs(g_vel[ax]) > DEADZONE) {
            double v = g_vel[ax];
            int steps;
            int delay;
            g_glide_until[ax] = 0;
            g_prev[ax] = v;
            g_moving = 1;
            if (now < g_next_fire[ax]) continue;
            steps = my_round(v * (double)CHUNK_STEPS);
            if (steps == 0) steps = (v > 0) ? 1 : -1;
            ptz_move(ax, steps);
            delay = (int)((double)CHUNK_MS * (my_fabs(v) > 0.3 ? my_fabs(v) : 0.3));
            g_next_fire[ax] = now + delay;
        } else if (GLIDE_MS > 0) {
            if (g_glide_until[ax] == 0) {
                if (my_fabs(g_prev[ax]) <= DEADZONE) continue;
                g_glide_until[ax] = now + GLIDE_MS;
                g_moving = 1;
            }
            if (now >= g_glide_until[ax]) {
                g_glide_until[ax] = 0;
                g_prev[ax] = 0.0;
                ptz_stop_ax(ax);
            } else if (now >= g_next_fire[ax]) {
                double frac = (double)(g_glide_until[ax] - now) / (double)GLIDE_MS;
                int steps = my_round(my_fabs(g_prev[ax]) * (double)CHUNK_STEPS * frac);
                if (steps >= 1) ptz_move(ax, g_prev[ax] > 0 ? steps : -steps);
                g_next_fire[ax] = now + TICK_MS;
            }
        } else {
            if (my_fabs(g_prev[ax]) > DEADZONE) {
                g_prev[ax] = 0.0;
                ptz_stop_ax(ax);
            }
        }
    }
    if (g_goto_active) g_moving = 1;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void load_config(void)
{
    FILE *f = fopen(g_conf_path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *nl;
        if (line[0] == '#') continue;
        if (!eq) continue;
        *eq = 0;
        nl = strpbrk(eq + 1, "\r\n");
        if (nl) *nl = 0;
        if (!strcmp(line, "port"))            g_port = atoi(eq + 1);
        else if (!strcmp(line, "ptzd_port"))  g_ptzd_port = atoi(eq + 1);
        else if (!strcmp(line, "iface"))    { snprintf(g_iface, sizeof g_iface, "%s", eq + 1); g_iface_set = 1; }
        else if (!strcmp(line, "ip"))         snprintf(g_ip, sizeof g_ip, "%s", eq + 1);
        else if (!strcmp(line, "tilt_sign"))  g_tilt_sign = (atof(eq + 1) < 0) ? -1.0 : 1.0;
    }
    fclose(f);
}

static void usage(void)
{
    printf(APP_NAME " " APP_VER " — démon ONVIF embarqué (device/media/PTZ)\n"
           "usage: " APP_NAME " [options]\n"
           "  --conf PATH        fichier de config (défaut %s)\n"
           "  --ip A.B.C.D       force l'IP annoncée (défaut: eth0)\n"
           "  --port N           port HTTP ONVIF (défaut 8080)\n"
           "  --ptzd-port N      port ptzd local (défaut 8091)\n"
           "  --self-test        exécute la table de tests XML et sort\n"
           "  --version          affiche la version\n"
           "  --help             cette aide\n", g_conf_path);
}

int main(int argc, char **argv)
{
    int i, listen_fd = -1;
    struct sockaddr_in sa;
    int opt_ip = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test"))          return selftest();
        else if (!strcmp(argv[i], "--version"))       { printf(APP_NAME " " APP_VER "\n"); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--foreground"))    { /* no-op */ }
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) g_conf_path = argv[++i];
        else if (!strcmp(argv[i], "--ip") && i + 1 < argc) { snprintf(g_ip, sizeof g_ip, "%s", argv[++i]); opt_ip = 1; }
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ptzd-port") && i + 1 < argc) g_ptzd_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc) { snprintf(g_iface, sizeof g_iface, "%s", argv[++i]); g_iface_set = 1; }
        else { printf("option inconnue: %s\n", argv[i]); usage(); return 2; }
    }

    load_config();
    if (!g_iface_set) pick_iface(g_iface, sizeof g_iface);
    if (!opt_ip || !g_ip[0]) {
        char tmp[32];
        if (iface_ip(g_iface, tmp, sizeof tmp) == 0) snprintf(g_ip, sizeof g_ip, "%s", tmp);
    }
    load_identity();
    preset_load();
    { struct preset *pl = preset_find("level");
      if (pl) tilt_ref_note(pl->tilt); }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    logmsg(APP_NAME " " APP_VER " starting: ip=%s port=%d ptzd=127.0.0.1:%d fw=%s serial=%s tilt_sign=%+.0f",
           g_ip, g_port, g_ptzd_port, g_fwver, g_serial, g_tilt_sign);

    probe_streams();
    ptz_connect();
    /* Au boot la camera reste souvent en position repliee : on la remet sur sa
     * vue de veille si elle a ete enregistree, sinon elle regarde le plafond. */
    { struct preset *pl = preset_find("level");
      if (pl && pl->tilt > 0) {
          logmsg("boot: retour au preset level (%d,%d)", pl->pan, pl->tilt);
          goto_preset_by_name("level");
      } }
    logmsg("ptzd: %s", g_ptz_fd >= 0 ? "connecté" : "indisponible (retry à la demande)");

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { logmsg("socket: %s", strerror(errno)); return 1; }
    {
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        logmsg("bind %d: %s", g_port, strerror(errno));
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 8) != 0) {
        logmsg("listen: %s", strerror(errno));
        close(listen_fd);
        return 1;
    }
    logmsg("HTTP ONVIF à l'écoute sur 0.0.0.0:%d", g_port);

    g_start_ms = mono_ms();
    g_last_cmd_ms = g_start_ms;
    {
        long long next_ping = g_start_ms + PING_MS;
        while (!g_stop) {
            struct pollfd pfd;
            int pr;
            long long now;

            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pr = poll(&pfd, 1, TICK_MS);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in ca;
                socklen_t cl = sizeof ca;
                int cfd = accept(listen_fd, (struct sockaddr *)&ca, &cl);
                if (cfd >= 0) {
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    handle_conn(cfd);
                    close(cfd);
                }
            } else if (pr < 0 && errno != EINTR) {
                logmsg("poll: %s", strerror(errno));
                break;
            }

            now = mono_ms();
            tick();

            if (now >= next_ping) {
                next_ping = now + PING_MS;
                if (!ptz_ping()) logmsg("ptzd keepalive KO (reconnectera)");
            }
        }
    }

    logmsg("arrêt demandé");
    motor_halt();
    goto_cancel();
    ptz_stop_all();
    ptz_close();
    close(listen_fd);
    return 0;
}
