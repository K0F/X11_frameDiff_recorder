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

#define WIDTH 720
#define HEIGHT 576

int main() {
    // 1. FFmpeg Setup - Open the pipe early

char monitor_name[256] = "default";
FILE *p = popen("pactl get-default-sink", "r");
if (p) {
    if (fgets(monitor_name, sizeof(monitor_name), p)) {
        // Remove newline
        monitor_name[strcspn(monitor_name, "\n")] = 0;
        // Append .monitor
        strcat(monitor_name, ".monitor");
    }
    pclose(p);
}

// Then use monitor_name in your main ffmpeg popen string:
char cmd[1024];
sprintf(cmd, "ffmpeg -y -use_wallclock_as_timestamps 1 "
             "-f rawvideo -pixel_format bgra -video_size 720x576 -framerate 50 -i - "
             "-f pulse -i %s "
             "-c:v libx264 -preset ultrafast -tune zerolatency "
             "-c:a aac -b:a 192k -af aresample=async=1 "
             "-pix_fmt yuv420p -shortest output.mp4", monitor_name);

FILE *ffmpeg = popen(cmd, "w");

    
	// pic only
    //FILE *ffmpeg = popen("ffmpeg -y -f rawvideo -pixel_format bgra -video_size 720x576 -re -i - -c:v libx264 -preset ultrafast -pix_fmt yuv420p output.mp4", "w");
    if (!ffmpeg) {
        fprintf(stderr, "Failed to open ffmpeg pipe\n");
        return 1;
    }

    Display *display = XOpenDisplay(NULL);
    if (!display) return 1;

    Window root = DefaultRootWindow(display);
    int screen_width = DisplayWidth(display, DefaultScreen(display));
    int screen_height = DisplayHeight(display, DefaultScreen(display));

    // OpenGL & Window Setup
    GLint att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
    XVisualInfo *vi = glXChooseVisual(display, 0, att);
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(display, root, vi->visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask;
    Window win = XCreateWindow(display, root, 100, 100, WIDTH, HEIGHT, 0, vi->depth, 
                              InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(display, win);
    GLXContext glc = glXCreateContext(display, vi, NULL, GL_TRUE);
    glXMakeCurrent(display, win, glc);

    // MIT-SHM Setup
    XShmSegmentInfo shminfo;
    XImage *img = XShmCreateImage(display, vi->visual, vi->depth, ZPixmap, NULL, &shminfo, WIDTH, HEIGHT);
    shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
    shminfo.shmaddr = img->data = shmat(shminfo.shmid, 0, 0);
    shminfo.readOnly = False;
    XShmAttach(display, &shminfo);

    // Texture setup
    glEnable(GL_TEXTURE_2D);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    unsigned char *prev_buffer = calloc(WIDTH * HEIGHT * 4, 1);
    unsigned char *diff_buffer = malloc(WIDTH * HEIGHT * 4);

    XEvent xev;
    int running = 1;
    
    while (running) {
        while (XPending(display)) {
            XNextEvent(display, &xev);
            if (xev.type == KeyPress) running = 0;
        }

        // Mouse Capture Logic
        Window r_ret, c_ret;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(display, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask);

        int cap_x = rx - (WIDTH / 2);
        int cap_y = ry - (HEIGHT / 2);

        // Fixes the "misleading indentation" warning
        if (cap_x < 0) cap_x = 0; 
        if (cap_y < 0) cap_y = 0;
        if (cap_x + WIDTH > screen_width) cap_x = screen_width - WIDTH;
        if (cap_y + HEIGHT > screen_height) cap_y = screen_height - HEIGHT;

        // 4. Capture and B&W Difference Logic
        XShmGetImage(display, root, img, cap_x, cap_y, AllPlanes);
        unsigned char *src = (unsigned char *)img->data;

uint32_t *src32 = (uint32_t *)img->data;
uint32_t *prev32 = (uint32_t *)prev_buffer;
uint32_t *diff32 = (uint32_t *)diff_buffer;

for (int i = 0; i < WIDTH * HEIGHT; i++) {
    uint32_t s = src32[i];
    uint32_t p = prev32[i];

    // Fast absolute difference for each channel packed in the int
    // This is a rough but very fast way to get a grayscale-ish diff
    unsigned char b = abs((s & 0xFF) - (p & 0xFF));
    unsigned char g = abs(((s >> 8) & 0xFF) - ((p >> 8) & 0xFF));
    unsigned char r = abs(((s >> 16) & 0xFF) - ((p >> 16) & 0xFF));

	int temp_gray = (r + g + b); // No division = 3x brighter
	unsigned char gray = (temp_gray > 255) ? 255 : (unsigned char)temp_gray;
    
    //unsigned char gray = (r + g + b) / 3;
    
    // Pack it back into BGRA (0xFF for alpha)
    diff32[i] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
    prev32[i] = s;
}

		/* // color
        // Capture and Diff
        XShmGetImage(display, root, img, cap_x, cap_y, AllPlanes);
        unsigned char *src = (unsigned char *)img->data;
        for (int i = 0; i < WIDTH * HEIGHT * 4; i++) {
            diff_buffer[i] = abs(src[i] - prev_buffer[i]);
            prev_buffer[i] = src[i];
        }
        */


        // 5. Pipe to FFmpeg and Render
        fwrite(diff_buffer, 1, WIDTH * HEIGHT * 4, ffmpeg);
        

        // Update Texture and Render
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_BYTE, diff_buffer);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex2f(-1, -1);
            glTexCoord2f(1, 1); glVertex2f(1, -1);
            glTexCoord2f(1, 0); glVertex2f(1, 1);
            glTexCoord2f(0, 0); glVertex2f(-1, 1);
        glEnd();
        glXSwapBuffers(display, win);
    }

    // --- Cleanup: Finalizing everything ---
    pclose(ffmpeg); // This closes the pipe and saves the file correctly
    XShmDetach(display, &shminfo);
    XDestroyImage(img);
    shmdt(shminfo.shmaddr);
    shmctl(shminfo.shmid, IPC_RMID, 0);
    free(prev_buffer); 
    free(diff_buffer);
    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, glc);
    XDestroyWindow(display, win);
    XCloseDisplay(display);

    return 0;
}
