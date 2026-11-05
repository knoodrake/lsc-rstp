/* ptzd — LSC PTZ ioctl daemon for cam_3 (Anyka AK3918EV330, ARMv5)
 * TCP :8091 text protocol, persistent motor fds, chunked finite moves,
 * camera-side dead-man via /tmp/ptzd.hb heartbeat file.
 *
 * Protocol (newline-terminated commands, one reply line each):
 *   MOVE <axis> <steps>   axis: 0=pan /dev/motor0, 1=tilt /dev/motor1;
 *                         finite steps, split into chunks of <=300 so the
 *                         daemon can react to STOP between chunks.
 *                         Async: returns OK immediately, motion continues.
 *   STOP [axis]           stop ioctl on one or both motors, any time.
 *   POS                   GET_STATE both: "PAN pos=<n> state=<n> TILT pos=<n> state=<n>"
 *   PING                  -> PONG (also refreshes heartbeat)
 *   --stop-now            (no listen) open, STOP both, exit 0. Watchdog helper.
 *
 * Heartbeat: /tmp/ptzd.hb gets the epoch on every command received.
 * Supervisor (ptzd-supervise.sh) STOPs + restarts us if the shim goes silent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define IOC_MOVE   0x40046d40u
#define IOC_SPEED  0x40046d41u
#define IOC_STOP   0x40046d42u
#define IOC_STATE  0x40046d43u
#define HB_PATH    "/tmp/ptzd.hb"
#define CHUNK      300          /* steps per MOVE ioctl (matrix: kernel caps ~380) */
#define MAXCHUNKS  64           /* so worst-case runtime < deadman */
#define IDLE_TIMEOUT 30         /* sec without a command: free the slot */

static int fdm[2] = { -1, -1 };

static int motor_open(int i)
{
    if (fdm[i] >= 0) return 0;
    fdm[i] = open(i ? "/dev/motor1" : "/dev/motor0", O_RDWR | O_NONBLOCK);
    return fdm[i] >= 0 ? 0 : -errno;
}

static void heartbeat(void)
{
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%ld\n", (long)time(NULL));
    int fd = open(HB_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { ssize_t w = write(fd, buf, n); (void)w; close(fd); }
}

/* stop both; cross-fd, instant, idempotent */
static void stop_all(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (motor_open(i) == 0)
            ioctl(fdm[i], IOC_STOP, 0);
}

/* one open() + STOP for watchdog helper (may block until idle; acceptable) */
static int stop_now(void)
{
    int i, r = 0;
    for (i = 0; i < 2; i++) {
        int fd = open(i ? "/dev/motor1" : "/dev/motor0", O_RDWR); /* blocking ok */
        if (fd < 0) { fprintf(stderr, "open %d fail %d\n", i, errno); r = -1; continue; }
        ioctl(fd, IOC_STOP, 0);
        close(fd);
    }
    return r;
}

static void do_pos(char *out, int cap)
{
    unsigned int st[8];
    int i, n = 0;
    n += snprintf(out + n, cap - n, "PAN");
    for (i = 0; i < 2; i++) {
        memset(st, 0, sizeof st);
        int rc = (motor_open(i) == 0) ? ioctl(fdm[i], IOC_STATE, st) : -1;
        n += snprintf(out + n, cap - n, " pos=%d state=%d rc=%d",
                      (int)st[1], (int)st[0], rc == 0 ? 0 : -errno);
        if (i == 0) n += snprintf(out + n, cap - n, " TILT");
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--stop-now"))
        return stop_now() ? 1 : 0;

    if (motor_open(0) || motor_open(1)) {
        fprintf(stderr, "motor open fail (errno pan=%d tilt=%d)\n", errno, errno);
        /* keep retrying via supervisor */
        return 1;
    }
    stop_all(); /* STOP-on-connect equivalent at boot */

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return 1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(8091);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 4)) {
        fprintf(stderr, "bind/listen fail errno=%d\n", errno);
        return 1;
    }

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        FILE *fp = fdopen(c, "r+");
        if (!fp) { close(c); continue; }
        char line[128];
        heartbeat();
        for (;;) {
            /* idle guard: never let a slot be held forever by a leaked conn */
            fd_set rs; FD_ZERO(&rs); FD_SET(c, &rs);
            struct timeval tv = { IDLE_TIMEOUT, 0 };
            int r = select(c + 1, &rs, NULL, NULL, &tv);
            if (r <= 0) break;
            if (!fgets(line, sizeof line, fp)) break;
            heartbeat();
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "PING", 4)) {
                fputs("PONG\n", fp); fflush(fp);
            } else if (!strncmp(line, "STOP", 4)) {
                int ax = -1;
                if (line[4] == ' ') ax = atoi(line + 5);
                if (ax == 0 || ax == 1) ioctl(fdm[ax], IOC_STOP, 0);
                else stop_all();
                fputs("OK\n", fp); fflush(fp);
            } else if (!strncmp(line, "POS", 3)) {
                char out[192];
                do_pos(out, sizeof out);
                fputs(out, fp); fputs("\n", fp); fflush(fp);
            } else if (!strncmp(line, "MOVE", 4)) {
                int ax = -1, steps = 0;
                if (sscanf(line + 4, "%d %d", &ax, &steps) == 2 && (ax == 0 || ax == 1)) {
                    /* chunked finite move; async: hand to one ioctl per chunk in
                     * a detached manner is overkill — kernel queues, so issue
                     * chunks back-to-back (each returns <30 ms). */
                    int rem = steps < 0 ? -steps : steps, sgn = steps < 0 ? -1 : 1;
                    int rc = 0;
                    while (rem > 0 && rc == 0) {
                        int ch = rem > CHUNK ? CHUNK : rem;
                        int s = sgn * ch;
                        rc = ioctl(fdm[ax], IOC_MOVE, &s) == 0 ? 0 : -errno;
                        rem -= ch;
                    }
                    if (rc == 0) fputs("OK\n", fp);
                    else fprintf(fp, "ERR %d\n", rc);
                    fflush(fp);
                } else fputs("ERR usage MOVE <axis> <steps>\n", fp);
            } else if (!strncmp(line, "BYE", 3)) {
                fputs("BYE\n", fp); fflush(fp);
                break;             /* polite: free this slot now */
            } else if (line[0]) {
                fputs("ERR unknown\n", fp); fflush(fp);
            }
        }
        fclose(fp); heartbeat(); /* slot freed */
        /* client died: motors keep last queued chunk (self-limiting) */
    }
    return 0;
}
