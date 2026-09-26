/*
 * Watch the face animate in a browser, with nothing installed.
 *
 * LVGL has an SDL backend, but a container has no window system and a VM usually has
 * no GUI at all. So the framebuffer is served over plain HTTP instead: the page asks
 * for a frame, this renders one and hands back the raw RGB565 bytes, and the canvas
 * converts them. No JPEG encoder, no WebSocket handshake, no dependencies — and the
 * browser's polling sets the frame rate, so there is no clock to keep in sync.
 */
#include "sim_server.h"
#include "face_view.h"
#include "face_motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define STATE_MS 6000   /* matches main.c, so the simulator cycles as the kit does */

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><title>Buddy face</title>"
"<style>body{margin:0;display:grid;place-items:center;height:100vh;background:#14170f;"
"font:13px/1.5 ui-sans-serif,system-ui,sans-serif;color:#cfd8c0}"
"canvas{width:480px;height:480px;image-rendering:pixelated;border-radius:50%;"
"box-shadow:0 0 0 10px #22261b,0 14px 40px #000a}"
"p{opacity:.65;margin:14px 0 0}</style>"
"<canvas id=c width=240 height=240></canvas><p id=s>connecting...</p><script>"
"const c=document.getElementById('c'),x=c.getContext('2d'),img=x.createImageData(240,240);"
"async function f(){try{const r=await fetch('/frame',{cache:'no-store'});"
"const st=r.headers.get('X-State')||'';const b=new Uint8Array(await r.arrayBuffer());"
"for(let i=0,j=0;i<240*240;i++,j+=4){const v=b[i*2]|(b[i*2+1]<<8);"
"img.data[j]=((v>>11)&31)<<3;img.data[j+1]=((v>>5)&63)<<2;img.data[j+2]=(v&31)<<3;img.data[j+3]=255;}"
"x.putImageData(img,0,0);document.getElementById('s').textContent=st;}"
"catch(e){document.getElementById('s').textContent='disconnected';}"
"requestAnimationFrame(f);}f();</script>";

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { if (errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

int sim_serve(int port, const uint16_t *fb, size_t fb_bytes, sim_render_cb render)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 8) < 0) { perror("listen"); return 1; }

    printf("face simulator on http://localhost:%d/  (ctrl-c to stop)\n", port);
    fflush(stdout);

    const uint32_t origin = now_ms();
    int shown = -1;

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char req[1024] = {0};
        ssize_t n = read(fd, req, sizeof(req) - 1);
        if (n <= 0) { close(fd); continue; }

        if (strncmp(req, "GET /frame", 10) == 0) {
            uint32_t ms = now_ms() - origin;
            int state = (int)((ms / STATE_MS) % (uint32_t)face_state_count);
            if (state != shown) { shown = state; face_view_set_state(state); }
            render(ms);

            char head[256];
            int hn = snprintf(head, sizeof(head),
                "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                "Content-Length: %zu\r\nX-State: %s %ums\r\nCache-Control: no-store\r\n"
                "Connection: close\r\n\r\n",
                fb_bytes, face_states[state].name, (unsigned)(ms % face_states[state].duration));
            send_all(fd, head, (size_t)hn);
            send_all(fd, fb, fb_bytes);
        } else {
            char head[256];
            int hn = snprintf(head, sizeof(head),
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n", sizeof(PAGE) - 1);
            send_all(fd, head, (size_t)hn);
            send_all(fd, PAGE, sizeof(PAGE) - 1);
        }
        close(fd);
    }
    close(srv);
    return 0;
}
