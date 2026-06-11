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
#include <time.h>
#include <stdint.h>

#define WIDTH 932 // silver PAL ratio format
#define HEIGHT 576
#define TARGET_FPS 60

#define FRAME_DIFF false // doing frame-differencing at all

// Extension function pointer for disabling V-Sync
typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display* dpy, GLXDrawable drawable, int interval);

int main() {
  // --- 1. Audio & FFmpeg Configuration ---
  // Natvrdo definovaný monitor výstupu (bez nespolehlivého runtime hledání)
  char monitor_name[256] = "alsa_output.pci-0000_00_1b.0.analog-stereo.monitor";

  // Generování UTC časového razítka pro název souboru
  time_t now = time(NULL);
  struct tm *utc_time = gmtime(&now);
  char timestamp[64];
  strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", utc_time);

  char filename[128];
  if (FRAME_DIFF) {
    snprintf(filename, sizeof(filename), "%s_diffrec.mp4", timestamp);
  } else {
    snprintf(filename, sizeof(filename), "%s_rec.mp4", timestamp);
  }

  // Sestavení FFmpeg příkazu s korektním mapováním monitoru a potlačením startovního bufferu
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), 
      "ffmpeg -y -fflags nobuffer+genpts "
      "-f rawvideo -pixel_format bgra -video_size %dx%d -framerate %d -i - "
      "-f pulse -thread_queue_size 2048 -fragment_size 1024 -i %s "
      "-c:v libx264 -preset ultrafast -tune zerolatency "
      "-c:a aac -b:a 192k -af \"aresample=async=1\" "
      "-pix_fmt yuv420p -qp 20 -r %d %s", 
      WIDTH, HEIGHT, TARGET_FPS, monitor_name, TARGET_FPS, filename);

  // Otevření roury pro real-time krmení FFmpegu
  FILE *ffmpeg = popen(cmd, "w");
  if (!ffmpeg) {
    fprintf(stderr, "Failed to open ffmpeg pipe\n");
    return 1;
  }

  // --- 2. X11 & OpenGL Setup ---
  Display *display = XOpenDisplay(NULL);
  if (!display) {
    pclose(ffmpeg);
    return 1;
  }

  Window root = DefaultRootWindow(display);
  int screen_width = DisplayWidth(display, DefaultScreen(display));
  int screen_height = DisplayHeight(display, DefaultScreen(display));

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

  // Deaktivace V-Sync (GPU nebude brzdit smyčku na obnovovací frekvenci monitoru)
  PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
  if (glXSwapIntervalEXT) {
    glXSwapIntervalEXT(display, win, 0); 
  }

  // --- 3. MIT-SHM (Sdílená paměť) Setup ---
  XShmSegmentInfo shminfo;
  XImage *img = XShmCreateImage(display, vi->visual, vi->depth, ZPixmap, NULL, &shminfo, WIDTH, HEIGHT);
  shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
  shminfo.shmaddr = img->data = shmat(shminfo.shmid, 0, 0);
  shminfo.readOnly = False;
  XShmAttach(display, &shminfo);

  // --- 4. GL Texture Setup ---
  glEnable(GL_TEXTURE_2D);
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  unsigned char *prev_buffer = calloc(WIDTH * HEIGHT * 4, 1);
  unsigned char *diff_buffer = malloc(WIDTH * HEIGHT * 4);

  // Časovače pro stabilní FPS pacing
  struct timespec next_frame;
  clock_gettime(CLOCK_MONOTONIC, &next_frame);
  long frame_delay_ns = 1000000000 / TARGET_FPS; 

  XEvent xev;
  int running = 1;

  // --- 5. Main Loop ---
  while (running) {
    while (XPending(display)) {
      XNextEvent(display, &xev);
      if (xev.type == KeyPress) running = 0;
    }

    // Centrování zachytávané oblasti okolo kurzoru myši
    Window r_ret, c_ret;
    int rx, ry, wx, wy;
    unsigned int mask;
    XQueryPointer(display, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask);

    int cap_x = rx - (WIDTH / 2);
    int cap_y = ry - (HEIGHT / 2);

    if (cap_x < 0) cap_x = 0; 
    if (cap_y < 0) cap_y = 0;
    if (cap_x + WIDTH > screen_width) cap_x = screen_width - WIDTH;
    if (cap_y + HEIGHT > screen_height) cap_y = screen_height - HEIGHT;

    // Grabnutí obrazu do sdílené paměti
    XShmGetImage(display, root, img, cap_x, cap_y, AllPlanes);

    uint32_t *prev32 = (uint32_t *)prev_buffer;
    uint32_t *diff32 = (uint32_t *)diff_buffer;

    if (FRAME_DIFF) {
      // Diferenční režim (bezpečné řádkování přes bytes_per_line)
      for (int y = 0; y < HEIGHT; y++) {
        uint32_t *src_row = (uint32_t *)((char *)img->data + y * img->bytes_per_line);
        uint32_t *prev_row = prev32 + (y * WIDTH);
        uint32_t *diff_row = diff32 + (y * WIDTH);

        for (int x = 0; x < WIDTH; x++) {
          uint32_t s = src_row[x];
          uint32_t p = prev_row[x];

          unsigned char b = abs((int)(s & 0xFF) - (int)(p & 0xFF));
          unsigned char g = abs((int)((s >> 8) & 0xFF) - (int)((p >> 8) & 0xFF));
          unsigned char r = abs((int)((s >> 16) & 0xFF) - (int)((p >> 16) & 0xFF));

          int temp_gray = (r + g + b); 
          unsigned char gray = (temp_gray > 255) ? 255 : (unsigned char)temp_gray;

          diff_row[x] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
          prev_row[x] = s;
        }
      }

      fwrite(diff_buffer, 1, WIDTH * HEIGHT * 4, ffmpeg);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_BYTE, diff_buffer);

    } else {
      // Přímé kopírování (bezpečné řádkování eliminující deformace nesymetrických šířek)
      for (int y = 0; y < HEIGHT; y++) {
        uint32_t *src_row = (uint32_t *)((char *)img->data + y * img->bytes_per_line);
        uint32_t *prev_row = prev32 + (y * WIDTH);
        
        for (int x = 0; x < WIDTH; x++) {
          prev_row[x] = src_row[x];
        }
      }

      fwrite(prev_buffer, 1, WIDTH * HEIGHT * 4, ffmpeg);     
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_BYTE, prev_buffer);
    }

    // OpenGL překreslení textury do okna
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glEnd();
    glXSwapBuffers(display, win);

    // Přesný nanosekundový sleep pro udržení cílového framerate
    next_frame.tv_nsec += frame_delay_ns;
    while (next_frame.tv_nsec >= 1000000000) {
      next_frame.tv_sec++;
      next_frame.tv_nsec -= 1000000000;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
  }

  // --- 6. Clean and Graceful Shutdown ---
  fprintf(stderr, "Zavírám okno, ukončuji streamy...\n");
  fflush(ffmpeg);
  
  // Poslat FFmpegu korektní Ctrl+C signál, aby zapsal moov indexy a nezůstal viset jako zombie
  system("killall -INT ffmpeg"); 
  pclose(ffmpeg);

  // Uvolnění paměti a X11 struktur
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
