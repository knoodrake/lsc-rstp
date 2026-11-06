/* apweb.c — mini serveur HTTP de provisioning WiFi pour les caméras LSC PTZ (P5).
 *
 * Rôle : la caméra démarre en point d'accès (hostapd sur 172.14.10.1, udhcpd),
 * l'utilisateur s'y connecte, ouvre http://172.14.10.1/ et saisit le SSID / mot de
 * passe de SON réseau (+ éventuellement une IP statique) ; le serveur écrit
 * /etc/config/_ak39_factory.ini — le fichier INI que le firmware lit au boot
 * (mécanisme natif prouvé : cf. kit/wifi_card/ et P5_AP_WEB_FEASIBILITY.md).
 *
 * Contraintes du terrain (uClibc 1.0.28, ARMv5, busybox sans httpd) :
 *   - aucune dépendance hors libc, pas d'allocation dynamique pour les requêtes ;
 *   - une connexion à la fois, timeouts partout (une caméra n'a pas de watchdog HTTP) ;
 *   - corps de requête borné (8 Kio), pas de chunked, pas de keep-alive ;
 *   - écriture atomique du fichier (tmp + fsync + rename) pour ne jamais laisser
 *     un INI tronqué que le prochain boot lirait.
 *
 * Usage :
 *   apweb [PORT] [FICHIER_INI]     (défauts : 80 et /etc/config/_ak39_factory.ini)
 *   apweb --self-test 'ssid=..&passphrase=..&ip=..'   (écrit /tmp/apweb_selftest.ini
 *       et l'affiche, sans réseau ; APWEB_OUT=chemin pour choisir le fichier)
 *
 * Pages : GET /  -> formulaire ; POST /save -> écrit l'INI puis propose le reboot ;
 *         POST /reboot -> sync + reboot (RB_AUTOBOOT).
 * Construit par kit/p5_hostapd/build_apweb.sh (hôte ET ARM).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXREQ   8192
#define MAXFIELD 128

static const char *g_ini = "/etc/config/_ak39_factory.ini";
static int g_port = 80;

/* ---------- utilitaires ---------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* décode application/x-www-form-urlencoded ; renvoie la longueur ou -1 */
static int urldecode(const char *src, int n, char *out, int outsz)
{
    int i, o = 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n) {
            int h = hexval(src[i + 1]), l = hexval(src[i + 2]);
            if (h < 0 || l < 0) return -1;
            c = (h << 4) | l;
            i += 2;
        }
        if (c == '\0') c = ' ';
        if (o >= outsz - 1) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

/* extrait un champ du corps urlencodé ; 1 = trouvé, 0 = absent, -1 = invalide */
static int form_get(const char *body, const char *key, char *out, int outsz)
{
    int kl = (int)strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp;
        int seglen;
        if (strncmp(p, key, (size_t)kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            amp = strchr(v, '&');
            seglen = amp ? (int)(amp - v) : (int)strlen(v);
            return urldecode(v, seglen, out, outsz) < 0 ? -1 : 1;
        }
        amp = strchr(p, '&');
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static int only_digits_dots(const char *s, int maxlen)
{
    int i, n = 0;
    for (i = 0; s[i]; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) return 0;
        if (++n > maxlen) return 0;
    }
    return n > 0;
}

static int printable(const char *s, int minlen, int maxlen)
{
    int i;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return 0;  /* UTF-8 accepté */
    }
    return i >= minlen && i <= maxlen;
}

static void html_escape(const char *in, char *out, int outsz)
{
    int o = 0;
    for (; *in && o < outsz - 6; in++) {
        switch (*in) {
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default: out[o++] = *in;
        }
    }
    out[o] = '\0';
}

/* écriture atomique de l'INI */
static int write_ini(const char *ssid, const char *pass, const char *ip,
                     const char *nm, const char *gw, char *err, int errsz)
{
    char tmp[256];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini);
    f = fopen(tmp, "w");
    if (!f) { snprintf(err, (size_t)errsz, "fopen(%s): %s", tmp, strerror(errno)); return -1; }
    fprintf(f, "[wifi_station]\nssid=%s\npassphrase=%s\n", ssid, pass);
    if (ip && ip[0]) fprintf(f, "ip=%s\n", ip);
    if (nm && nm[0]) fprintf(f, "netmask=%s\n", nm);
    if (gw && gw[0]) fprintf(f, "gateway=%s\n", gw);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, g_ini) != 0) {
        snprintf(err, (size_t)errsz, "rename(%s): %s", g_ini, strerror(errno));
        return -1;
    }
    sync();
    return 0;
}

/* ---------- HTTP ---------- */

static const char PAGE_HEAD[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\nCache-Control: no-store\r\n\r\n"
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LSC PTZ - WiFi</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#14161a;color:#e8eaed}"
    "main{max-width:34rem;margin:6vh auto;padding:1.4rem 1.6rem;background:#1e2128;"
    "border-radius:14px;box-shadow:0 10px 30px #0006}"
    "h1{font-size:1.25rem;margin:0 0 .3rem}p.sub{margin:0 0 1.2rem;color:#9aa0a6;font-size:.9rem}"
    "label{display:block;margin:.9rem 0 .25rem;font-size:.85rem;color:#b8bcc4}"
    "input{width:100%;box-sizing:border-box;padding:.6rem .7rem;border-radius:8px;"
    "border:1px solid #34383f;background:#0f1115;color:#e8eaed;font-size:1rem}"
    "button{margin-top:1.3rem;width:100%;padding:.75rem;border:0;border-radius:9px;"
    "background:#3b82f6;color:#fff;font-size:1rem;font-weight:600}"
    ".ok{background:#12351f;border:1px solid #1f6b3a;padding:.9rem;border-radius:9px;"
    "margin:1rem 0;font-size:.95rem}.err{background:#3a1414;border:1px solid #7a2b2b;"
    "padding:.9rem;border-radius:9px;margin:1rem 0;font-size:.95rem}"
    "code{background:#0f1115;padding:.15rem .35rem;border-radius:5px;font-size:.85rem}"
    "</style></head><body><main><h1>Configuration WiFi</h1>"
    "<p class='sub'>Caméra LSC PTZ &mdash; mode point d'accès</p>";

static const char PAGE_FORM[] =
    "<form method='post' action='/save'>"
    "<label>Nom du r&eacute;seau (SSID)</label>"
    "<input name='ssid' maxlength='32' required value='%s'>"
    "<label>Mot de passe WiFi</label>"
    "<input name='passphrase' type='password' minlength='8' maxlength='63' required>"
    "<label>Adresse IP fixe (optionnel)</label>"
    "<input name='ip' placeholder='192.168.1.50' value='%s'>"
    "<label>Masque (optionnel)</label><input name='netmask' placeholder='255.255.255.0' value='%s'>"
    "<label>Passerelle (optionnel)</label><input name='gateway' placeholder='192.168.1.1' value='%s'>"
    "<button type='submit'>Enregistrer et red&eacute;marrer</button></form>"
    "<p class='sub' style='margin-top:1.2rem'>Laissez le r&eacute;seau vide pour une IP par DHCP "
    "(si DHCP indisponible, l'IP du dernier INI est conserv&eacute;e).</p>"
    "</main></body></html>";

static const char PAGE_FOOT[] = "</main></body></html>";

static void send_all(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = (int)send(fd, buf + off, (size_t)(len - off), MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += n;
    }
}

static void page_ok(int fd, const char *ssid, const char *pass, const char *ip,
                    const char *nm, const char *gw)
{
    char body[8192], e1[MAXFIELD * 6], e2[MAXFIELD * 6], e3[MAXFIELD * 6];
    html_escape(ssid, e1, sizeof e1);
    html_escape(ip, e2, sizeof e2);
    html_escape(gw, e3, sizeof e3);
    (void)nm; (void)pass;
    snprintf(body, sizeof body,
             "%s<div class='ok'><b>Enregistr&eacute;.</b><br>SSID : <code>%s</code>"
             "%s%s%s<br>Le fichier <code>/etc/config/_ak39_factory.ini</code> est &eacute;crit "
             "et synchronis&eacute; sur la flash.<br>La cam&eacute;ra red&eacute;marre dans 2 s "
             "et rejoint ton r&eacute;seau.</div>"
             "<p class='sub'>Si elle ne revient pas : rebranche-la en Ethernet ou remets le "
             "point d'acc&egrave;s et recharge cette page.</p>"
             "<form method='post' action='/reboot'><button>Red&eacute;marrer maintenant</button>"
             "</form>%s",
             PAGE_HEAD, ssid,
             ip[0] ? "<br>IP fixe : <code>" : "", ip[0] ? e2 : "",
             ip[0] ? "</code>" : "", PAGE_FOOT);
    send_all(fd, body, (int)strlen(body));
    (void)e3;
}

static void page_err(int fd, const char *msg)
{
    char body[8192], e[MAXFIELD * 6];
    html_escape(msg, e, sizeof e);
    snprintf(body, sizeof body,
             "%s<div class='err'><b>Erreur :</b> %s</div>"
             "<p class='sub'><a href='/' style='color:#7aa2ff'>Revenir au formulaire</a></p>%s",
             PAGE_HEAD, e, PAGE_FOOT);
    send_all(fd, body, (int)strlen(body));
}

static void page_form(int fd, const char *ssid, const char *ip, const char *nm, const char *gw)
{
    char body[8192], e1[MAXFIELD * 6], e2[MAXFIELD * 6], e3[MAXFIELD * 6], e4[MAXFIELD * 6];
    html_escape(ssid, e1, sizeof e1);
    html_escape(ip, e2, sizeof e2);
    html_escape(nm, e3, sizeof e3);
    html_escape(gw, e4, sizeof e4);
    snprintf(body, sizeof body, "%s", PAGE_HEAD);
    {
        char form[4096];
        snprintf(form, sizeof form, PAGE_FORM, e1, e2, e3, e4);
        strncat(body, form, sizeof body - strlen(body) - 1);
    }
    strncat(body, PAGE_FOOT, sizeof body - strlen(body) - 1);
    send_all(fd, body, (int)strlen(body));
}

static int do_reboot(void)
{
    if (getenv("APWEB_NO_REBOOT")) {   /* host build: never reboot the host */
        fprintf(stderr, "[apweb] (APWEB_NO_REBOOT) reboot simule\n");
        return 0;
    }
    sync();
    sleep(1);
    reboot(RB_AUTOBOOT);
    return 1;
}

/* traite une connexion ; renvoie 1 si un reboot est demandé */
static int handle(int fd)
{
    char req[MAXREQ + 1], body[MAXREQ + 1];
    char *hdr_end, *line, *method, *path;
    char ssid[MAXFIELD] = "", pass[MAXFIELD] = "", ip[64] = "", nm[64] = "", gw[64] = "";
    int n, total = 0, clen = 0;
    char err[256];

    while (total < MAXREQ) {
        n = (int)recv(fd, req + total, (size_t)(MAXREQ - total), 0);
        if (n <= 0) break;
        total += n;
        req[total] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) {
            char *cl = strstr(req, "Content-Length:");
            if (!cl) cl = strstr(req, "content-length:");
            if (cl) {
                clen = atoi(cl + 15);
                if (clen < 0 || clen > MAXREQ) clen = 0;
            }
            if ((int)(total - (int)(hdr_end + 4 - req)) >= clen) break;
        }
    }
    if (!total) return 0;
    req[total] = '\0';
    hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) { page_err(fd, "requete incomplete"); return 0; }
    *hdr_end = '\0';
    body[0] = '\0';
    if (clen > 0) {
        int have = total - (int)(hdr_end + 4 - req);
        if (have > clen) have = clen;
        if (have > 0) { memcpy(body, hdr_end + 4, (size_t)have); body[have] = '\0'; }
    }

    line = strtok(req, "\r\n");
    if (!line) { page_err(fd, "requete vide"); return 0; }
    method = strtok(line, " ");
    path = strtok(NULL, " ");
    if (!method || !path) { page_err(fd, "ligne de requete invalide"); return 0; }

    if (!strcasecmp(method, "GET")) {
        page_form(fd, "", "", "255.255.255.0", "");
        return 0;
    }
    if (strcasecmp(method, "POST")) { page_err(fd, "methode non supportee"); return 0; }

    if (!strncmp(path, "/save", 5)) {
        if (form_get(body, "ssid", ssid, sizeof ssid) != 1) { page_err(fd, "SSID manquant"); return 0; }
        if (form_get(body, "passphrase", pass, sizeof pass) != 1) { page_err(fd, "mot de passe manquant"); return 0; }
        form_get(body, "ip", ip, sizeof ip);
        form_get(body, "netmask", nm, sizeof nm);
        form_get(body, "gateway", gw, sizeof gw);
        if (!printable(ssid, 1, 32)) { page_err(fd, "SSID invalide (1-32 caracteres imprimables)"); return 0; }
        if (!printable(pass, 8, 63)) { page_err(fd, "mot de passe invalide (8-63 caracteres)"); return 0; }
        if (ip[0] && !only_digits_dots(ip, 15)) { page_err(fd, "IP invalide"); return 0; }
        if (nm[0] && !only_digits_dots(nm, 15)) { page_err(fd, "masque invalide"); return 0; }
        if (gw[0] && !only_digits_dots(gw, 15)) { page_err(fd, "passerelle invalide"); return 0; }
        if (write_ini(ssid, pass, ip, nm, gw, err, sizeof err) != 0) { page_err(fd, err); return 0; }
        fprintf(stderr, "[apweb] INI ecrit : ssid=%s ip=%s\n", ssid, ip[0] ? ip : "dhcp");
        page_ok(fd, ssid, pass, ip, nm, gw);
        return 1;
    }
    if (!strncmp(path, "/reboot", 7)) {
        page_ok(fd, "(redemarrage demand&eacute;)", "", "", "", "");
        return 1;
    }
    page_err(fd, "chemin inconnu");
    return 0;
}

int main(int argc, char **argv)
{

    /* --self-test : parse un corps urlencodé, écrit l'INI, l'affiche */
    if (argc >= 3 && !strcmp(argv[1], "--self-test")) {
        char ssid[MAXFIELD] = "", pass[MAXFIELD] = "", ip[64] = "", nm[64] = "", gw[64] = "";
        char err[192];
        const char *env = getenv("APWEB_OUT");
        /* Le self-test ne doit JAMAIS ecrire l'INI de production : par defaut
         * il ecrit dans /tmp (APWEB_OUT permet de choisir un autre chemin). */
        g_ini = env ? env : "/tmp/apweb_selftest.ini";
        if (form_get(argv[2], "ssid", ssid, sizeof ssid) != 1 ||
            form_get(argv[2], "passphrase", pass, sizeof pass) != 1) {
            fprintf(stderr, "self-test: ssid/passphrase absents\n");
            return 2;
        }
        form_get(argv[2], "ip", ip, sizeof ip);
        form_get(argv[2], "netmask", nm, sizeof nm);
        form_get(argv[2], "gateway", gw, sizeof gw);
        if (!printable(ssid, 1, 32) || !printable(pass, 8, 63)) {
            fprintf(stderr, "self-test: valeurs invalides\n");
            return 3;
        }
        if (write_ini(ssid, pass, ip, nm, gw, err, sizeof err) != 0) {
            fprintf(stderr, "self-test: %s\n", err);
            return 4;
        }
        printf("SELF-TEST OK -> %s\n", g_ini);
        {
            FILE *f = fopen(g_ini, "r");
            char line[256];
            while (f && fgets(line, sizeof line, f)) fputs(line, stdout);
            if (f) fclose(f);
        }
        return 0;
    }

    if (argc >= 2) g_port = atoi(argv[1]);
    if (argc >= 3) g_ini = argv[2];
    if (g_port <= 0 || g_port > 65535) { fprintf(stderr, "port invalide\n"); return 2; }

    signal(SIGPIPE, SIG_IGN);

    {
        int s, one = 1;
        struct sockaddr_in a;
        struct timeval tv;
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) { perror("socket"); return 1; }
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((unsigned short)g_port);
        if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
        if (listen(s, 4) != 0) { perror("listen"); return 1; }
        fprintf(stderr, "[apweb] http://0.0.0.0:%d/  (INI=%s)\n", g_port, g_ini);
        tv.tv_sec = 15; tv.tv_usec = 0;
        for (;;) {
            struct sockaddr_in peer;
            socklen_t pl = sizeof peer;
            int c = accept(s, (struct sockaddr *)&peer, &pl);
            int want_reboot;
            if (c < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                sleep(1);
                continue;
            }
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            fprintf(stderr, "[apweb] connexion de %s\n", inet_ntoa(peer.sin_addr));
            want_reboot = handle(c);
            shutdown(c, SHUT_RDWR);
            close(c);
            if (want_reboot) {
                fprintf(stderr, "[apweb] reboot demande\n");
                if (do_reboot()) close(s);
            }
        }
    }
    return 0;
}
