#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#define WIDTH 720
#define HEIGHT 576
#define MIDDLE_LINE_OFFSET (288 * WIDTH)

int main() {
    // --- 1. Audio (ALSA) Setup ---
    snd_pcm_t *pcm_handle;
    if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "ALSA: Cannot open playback device\n");
        return 1;
    }
    // 8-bit Mono at 8kHz for grit. 10ms latency for zero lag.
    snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_U8, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 8000, 1, 10000);

    // --- 2. FFmpeg Setup ---
    char monitor_name[256] = "default";
    FILE *p_sink = popen("pactl get-default-sink", "r");
    if (p_sink) {
        if (fgets(monitor_name, sizeof(monitor_name), p_sink)) {
            monitor_name[strcspn(monitor_name, "\n")] = 0;
            strcat(monitor_name, ".monitor");
        }
        pclose(p_sink);
    }

    char cmd[1024];
    sprintf(cmd, "ffmpeg -y -use_wallclock_as_timestamps 1 "
                 "-f rawvideo -pixel_format bgra -video_size 720x576 -framerate 50 -i - "
                 "-f pulse -i %s "
                 "-c:v libx264 -preset superfast -tune zerolatency "
                 "-c:a aac -b:a 192k -af aresample=async=1 "
                 "-pix_fmt yuv420p -shortest output.mp4", monitor_name);

    FILE *ffmpeg = popen(cmd, "w");
    if (!ffmpeg) return 1;

    // --- 3. X11 & OpenGL Setup ---
    Display *display = XOpenDisplay(NULL);
    Window root = DefaultRootWindow(display);
    int screen_width = DisplayWidth(display, DefaultScreen(display));
    int screen_height = DisplayHeight(display, DefaultScreen(display));

    GLint att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
    XVisualInfo *vi = glXChooseVisual(display, 0, att);
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(display, root, vi->visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask;
    Window win = XCreateWindow(display, root, 100, 100, WIDTH, HEIGHT, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(display, win);
    GLXContext glc = glXCreateContext(display, vi, NULL, GL_TRUE);
    glXMakeCurrent(display, win, glc);

    // --- 4. MIT-SHM Setup ---
    XShmSegmentInfo shminfo;
    XImage *img = XShmCreateImage(display, vi->visual, vi->depth, ZPixmap, NULL, &shminfo, WIDTH, HEIGHT);
    shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
    shminfo.shmaddr = img->data = shmat(shminfo.shmid, 0, 0);
    shminfo.readOnly = False;
    XShmAttach(display, &shminfo);

    // --- 5. Texture & Viewport Config ---
    glEnable(GL_TEXTURE_2D);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glViewport(0, 0, WIDTH, HEIGHT);

    unsigned char *prev_buffer = calloc(WIDTH * HEIGHT * 4, 1);
    unsigned char *diff_buffer = malloc(WIDTH * HEIGHT * 4);
    
    // Shared memory for audio-visual inspection
    int shm_fd = shm_open("/sc_diff_buffer", O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, WIDTH * HEIGHT);
    unsigned char *shm_diff_ptr = mmap(0, WIDTH * HEIGHT, PROT_WRITE, MAP_SHARED, shm_fd, 0);

    XEvent xev;
    int running = 1;
    unsigned char audio_line[WIDTH];

    while (running) {
        while (XPending(display)) {
            XNextEvent(display, &xev);
            if (xev.type == KeyPress) running = 0;
        }

        // Capture Logic
        Window r_ret, c_ret;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(display, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask);

        int cap_x = rx - (WIDTH / 2);
        int cap_y = ry - (HEIGHT / 2);
        if (cap_x < 0) cap_x = 0; if (cap_y < 0) cap_y = 0;
        if (cap_x + WIDTH > screen_width) cap_x = screen_width - WIDTH;
        if (cap_y + HEIGHT > screen_height) cap_y = screen_height - HEIGHT;

        XShmGetImage(display, root, img, cap_x, cap_y, AllPlanes);
        
        uint32_t *src32 = (uint32_t *)img->data;
        uint32_t *prev32 = (uint32_t *)prev_buffer;
        uint32_t *diff32 = (uint32_t *)diff_buffer;

        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            uint32_t s = src32[i];
            uint32_t p = prev32[i];
            
            int db = abs((int)(s & 0xFF) - (int)(p & 0xFF));
            int dg = abs((int)((s >> 8) & 0xFF) - (int)((p >> 8) & 0xFF));
            int dr = abs((int)((s >> 16) & 0xFF) - (int)((p >> 16) & 0xFF));
            
            unsigned int temp_gray = dr + dg + db;
            unsigned char gray = (temp_gray > 255) ? 255 : (unsigned char)temp_gray;

            diff32[i] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
            shm_diff_ptr[i] = gray;
            prev32[i] = s;
        }

        // --- AUDIO: Direct Hardware Push ---
        memcpy(audio_line, shm_diff_ptr + MIDDLE_LINE_OFFSET, WIDTH);
        int err = snd_pcm_writei(pcm_handle, audio_line, WIDTH);
        if (err == -EPIPE) snd_pcm_prepare(pcm_handle);

        // --- VIDEO: FFmpeg Pipe ---
        fwrite(diff_buffer, 1, WIDTH * HEIGHT * 4, ffmpeg);

        // --- RENDER: OpenGL Display ---
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_BYTE, diff_buffer);

        glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex2f(-1, -1);
            glTexCoord2f(1, 1); glVertex2f(1, -1);
            glTexCoord2f(1, 0); glVertex2f(1, 1);
            glTexCoord2f(0, 0); glVertex2f(-1, 1);
        glEnd();

        glXSwapBuffers(display, win);
        glFlush();
    }

    // Cleanup
    pclose(ffmpeg);
    snd_pcm_close(pcm_handle);
    XShmDetach(display, &shminfo);
    XDestroyImage(img);
    shmdt(shminfo.shmaddr);
    shmctl(shminfo.shmid, IPC_RMID, 0);
    free(prev_buffer); 
    free(diff_buffer);
    XCloseDisplay(display);

    return 0;
}
