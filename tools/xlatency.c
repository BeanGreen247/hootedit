/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* xlatency: key-to-pixel typing latency of any X11 text editor, in the spirit of Typometer and Keypress.sh.
 *
 *   xlatency [-n COUNT] [-d DELAY_MS] [-r X,Y,W,H] [-t PIXELS] [-k KEY] [-w WARMUP] [-v]
 *   xlatency -P [-s START_EPOCH_MS] [-r X,Y,W,H] [-t PIXELS] [-T TIMEOUT_MS]     ready time instead: how long until the rectangle has been painted
 *
 * It sends KEY (default "a") through XTEST to whatever window has the focus, and polls the screen rectangle X,Y,W,H (root coordinates,
 * default 0,0,640,200) with shared-memory XGetImage until more than PIXELS pixels (default 6) differ from the frame before the key
 * went down. The time from the XTEST request to that frame is one sample. The caret blinking changes only a pixel or two, so the
 * threshold ignores it. Prints min / median / mean / p95 / p99 / max in milliseconds, one line, and the raw samples with -v.
 *
 * What it measures: the editor's input handling, layout, painting and the X server's copy of the pixels. It does not include the
 * monitor's scan-out. Run every editor on the same display (Xvfb is fine), same window size, same file, same font size.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    int count = 100, delay = 150, warm = 5, thresh = 6, verbose = 0, rx = 0, ry = 0, rw = 640, rh = 200, ready = 0, timeout_ms = 60000, thresh_set = 0;
    double start_epoch = 0;
    const char *keyname = "a";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) warm = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) { thresh = atoi(argv[++i]); thresh_set = 1; }
        else if (!strcmp(argv[i], "-P")) ready = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) start_epoch = atof(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keyname = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) sscanf(argv[++i], "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: xlatency [-n COUNT] [-d DELAY_MS] [-r X,Y,W,H] [-t PIXELS] [-k KEY] [-w WARMUP] [-v]\n"); return 2; }
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "xlatency: cannot open the display\n"); return 1; }
    int ev, er, maj, min;
    if (!XTestQueryExtension(dpy, &ev, &er, &maj, &min)) { fprintf(stderr, "xlatency: no XTEST extension\n"); return 1; }
    KeyCode kc = XKeysymToKeycode(dpy, XStringToKeysym(keyname));
    if (!kc) { fprintf(stderr, "xlatency: unknown key %s\n", keyname); return 1; }
    Window root = DefaultRootWindow(dpy);
    XShmSegmentInfo si;
    XImage *im = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap, NULL, &si, (unsigned)rw, (unsigned)rh);
    if (!im) { fprintf(stderr, "xlatency: no shared-memory image\n"); return 1; }
    si.shmid = shmget(IPC_PRIVATE, (size_t)im->bytes_per_line * (size_t)im->height, IPC_CREAT | 0600);
    si.shmaddr = im->data = shmat(si.shmid, 0, 0);
    si.readOnly = False;
    XShmAttach(dpy, &si);
    XSync(dpy, False);
    shmctl(si.shmid, IPC_RMID, 0);
    size_t bytes = (size_t)im->bytes_per_line * (size_t)im->height;
    if (ready) {   /* the window counts as painted once enough pixels differ from the first colour in the rectangle (a blank window is one colour) */
        if (!thresh_set) thresh = 3000;
        struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
        double real0 = (double)rt.tv_sec * 1e3 + (double)rt.tv_nsec * 1e-6, t0 = now_ms();
        double offset = start_epoch > 0 ? real0 - start_epoch : 0;   /* time already spent before we started looking */
        while (now_ms() - t0 < timeout_ms) {
            XShmGetImage(dpy, root, im, rx, ry, AllPlanes);
            const unsigned *a = (const unsigned *)im->data;
            unsigned first = a[0];
            int diff = 0;
            for (size_t k = 1; k < bytes / 4; k++) if (a[k] != first && ++diff > thresh) break;
            if (diff > thresh) { printf("ready=%.0f ms\n", now_ms() - t0 + offset); return 0; }
            usleep(2000);
        }
        printf("ready=timeout\n");
        return 1;
    }
    unsigned char *base = malloc(bytes);
    double *s = malloc(sizeof(double) * (size_t)count);
    int got = 0, misses = 0;
    for (int i = 0; i < count + warm; i++) {
        usleep((useconds_t)delay * 1000);
        XShmGetImage(dpy, root, im, rx, ry, AllPlanes);
        memcpy(base, im->data, bytes);
        double t0 = now_ms();
        XTestFakeKeyEvent(dpy, kc, True, 0);
        XTestFakeKeyEvent(dpy, kc, False, 0);
        XFlush(dpy);
        double dt = -1;
        while (now_ms() - t0 < 1000) {
            XShmGetImage(dpy, root, im, rx, ry, AllPlanes);
            int diff = 0;
            const unsigned *a = (const unsigned *)im->data, *b = (const unsigned *)base;
            for (size_t k = 0; k < bytes / 4; k++) if (a[k] != b[k] && ++diff > thresh) break;
            if (diff > thresh) { dt = now_ms() - t0; break; }
        }
        if (i < warm) continue;
        if (dt < 0) { misses++; continue; }
        s[got++] = dt;
        if (verbose) printf("%.2f\n", dt);
    }
    if (!got) { fprintf(stderr, "xlatency: no change seen in the rectangle (wrong -r, or the editor does not have the focus)\n"); return 1; }
    qsort(s, (size_t)got, sizeof(double), cmp);
    double sum = 0;
    for (int i = 0; i < got; i++) sum += s[i];
    printf("n=%d miss=%d min=%.1f median=%.1f mean=%.1f p95=%.1f p99=%.1f max=%.1f ms\n", got, misses, s[0], s[got / 2], sum / got, s[(int)((got - 1) * 0.95)], s[(int)((got - 1) * 0.99)], s[got - 1]);
    return 0;
}
