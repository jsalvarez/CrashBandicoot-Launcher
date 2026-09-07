#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FB_WIDTH 960
#define FB_HEIGHT 544
#define FB_STRIDE 960
#define FB_BLOCK_SIZE (2 * 1024 * 1024)
#define DATA_DIR "ux0:data/CrashRecomp"

static uint32_t *g_framebuffer;
static uint32_t *g_framebuffers[2];
static SceUID g_fb_blocks[2] = {-1, -1};

typedef struct {
    int dir_ok;
    int cue_count;
    int bin_count;
    int cue_read_ok;
    int bin_read_ok;
    char first_cue[256];
    char first_bin[256];
} DiscScan;

static inline uint32_t abgr(uint8_t a, uint8_t b, uint8_t g, uint8_t r) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static void clear_screen(uint32_t color) {
    for (int y = 0; y < FB_HEIGHT; ++y) {
        uint32_t *row = g_framebuffer + (y * FB_STRIDE);
        for (int x = 0; x < FB_WIDTH; ++x) row[x] = color;
    }
}

static void rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH) w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = y; yy < y + h; ++yy) {
        uint32_t *row = g_framebuffer + yy * FB_STRIDE;
        for (int xx = x; xx < x + w; ++xx) row[xx] = color;
    }
}

typedef struct { char ch; uint8_t rows[7]; } Glyph;
static const Glyph GLYPHS[] = {
    {' ', {0,0,0,0,0,0,0}}, {'-', {0,0,0,31,0,0,0}}, {'.', {0,0,0,0,0,6,6}}, {':', {0,6,6,0,6,6,0}}, {'/', {1,2,4,8,16,0,0}},
    {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}}, {'2', {14,17,1,2,4,8,31}}, {'3', {30,1,1,14,1,1,30}},
    {'4', {2,6,10,18,31,2,2}}, {'5', {31,16,16,30,1,1,30}}, {'6', {14,16,16,30,17,17,14}}, {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}}, {'9', {14,17,17,15,1,1,14}},
    {'A', {14,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}}, {'C', {14,17,16,16,16,17,14}}, {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}}, {'G', {14,17,16,23,17,17,14}}, {'H', {17,17,17,31,17,17,17}},
    {'I', {14,4,4,4,4,4,14}}, {'J', {7,2,2,2,18,18,12}}, {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}}, {'N', {17,25,21,19,17,17,17}}, {'O', {14,17,17,17,17,17,14}}, {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}}, {'S', {15,16,16,14,1,1,30}}, {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}}, {'V', {17,17,17,17,17,10,4}}, {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}}, {'Z', {31,1,2,4,8,16,31}}
};

static const uint8_t *glyph_for(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (unsigned i = 0; i < sizeof(GLYPHS)/sizeof(GLYPHS[0]); ++i) {
        if (GLYPHS[i].ch == c) return GLYPHS[i].rows;
    }
    return GLYPHS[0].rows;
}

static void draw_char(int x, int y, int scale, char c, uint32_t color) {
    const uint8_t *rows = glyph_for(c);
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < 5; ++gx) {
            if (rows[gy] & (1u << (4-gx))) {
                rect(x + gx*scale, y + gy*scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, int scale, const char *text, uint32_t color) {
    int start_x = x;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') { x = start_x; y += 9*scale; continue; }
        draw_char(x, y, scale, *p, color);
        x += 6*scale;
    }
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int ends_with_ci(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len) return 0;
    const char *tail = name + name_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        if (ascii_lower(tail[i]) != ascii_lower(suffix[i])) return 0;
    }
    return 1;
}

static int can_read_file(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;

    uint8_t probe[32];
    SceSSize got = sceIoRead(fd, probe, sizeof(probe));
    sceIoClose(fd);
    return got > 0;
}

static void scan_disc_files(DiscScan *scan) {
    memset(scan, 0, sizeof(*scan));

    /* The parent ux0:data directory already exists on a normal Vita. */
    sceIoMkdir(DATA_DIR, 0777);

    SceUID dir = sceIoDopen(DATA_DIR);
    if (dir < 0) return;
    scan->dir_ok = 1;

    for (;;) {
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        int rc = sceIoDread(dir, &entry);
        if (rc <= 0) break;
        if (!SCE_S_ISREG(entry.d_stat.st_mode)) continue;

        if (ends_with_ci(entry.d_name, ".cue")) {
            scan->cue_count++;
            if (scan->first_cue[0] == '\0') {
                snprintf(scan->first_cue, sizeof(scan->first_cue), "%s", entry.d_name);
            }
        } else if (ends_with_ci(entry.d_name, ".bin")) {
            scan->bin_count++;
            if (scan->first_bin[0] == '\0') {
                snprintf(scan->first_bin, sizeof(scan->first_bin), "%s", entry.d_name);
            }
        }
    }

    sceIoDclose(dir);

    if (scan->first_cue[0]) scan->cue_read_ok = can_read_file(scan->first_cue);
    if (scan->first_bin[0]) scan->bin_read_ok = can_read_file(scan->first_bin);
}

static int alloc_framebuffers(void) {
    for (int i = 0; i < 2; ++i) {
        g_fb_blocks[i] = sceKernelAllocMemBlock(
            i == 0 ? "crash_fb0" : "crash_fb1",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
            FB_BLOCK_SIZE,
            NULL
        );
        if (g_fb_blocks[i] < 0) return g_fb_blocks[i];

        void *base = NULL;
        int rc = sceKernelGetMemBlockBase(g_fb_blocks[i], &base);
        if (rc < 0) return rc;

        g_framebuffers[i] = (uint32_t *)base;
        memset(g_framebuffers[i], 0, FB_BLOCK_SIZE);
    }
    return 0;
}

static void free_framebuffers(void) {
    sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_IMMEDIATE);
    for (int i = 0; i < 2; ++i) {
        if (g_fb_blocks[i] >= 0) {
            sceKernelFreeMemBlock(g_fb_blocks[i]);
            g_fb_blocks[i] = -1;
        }
        g_framebuffers[i] = NULL;
    }
}

static void present(uint32_t *buffer) {
    SceDisplayFrameBuf fb = {
        .size = sizeof(SceDisplayFrameBuf),
        .base = buffer,
        .pitch = FB_STRIDE,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width = FB_WIDTH,
        .height = FB_HEIGHT
    };

    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (alloc_framebuffers() < 0) {
        free_framebuffers();
        sceKernelExitProcess(1);
        return 1;
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    DiscScan scan;
    scan_disc_files(&scan);

    int running = 1;
    int draw_index = 0;
    uint32_t frame = 0;
    uint32_t previous_buttons = 0;

    while (running) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        if ((pad.buttons & SCE_CTRL_TRIANGLE) && !(previous_buttons & SCE_CTRL_TRIANGLE)) {
            scan_disc_files(&scan);
        }

        g_framebuffer = g_framebuffers[draw_index];

        uint32_t bg = abgr(255,22,19,18);
        uint32_t panel = abgr(255,39,35,32);
        uint32_t text = abgr(255,235,235,235);
        uint32_t accent = abgr(255,70,163,255);
        uint32_t ok = abgr(255,90,210,120);
        uint32_t warn = abgr(255,90,185,245);
        uint32_t bad = abgr(255,85,85,235);

        int pair_ready = scan.cue_count > 0 && scan.bin_count > 0;
        int readable = pair_ready && scan.cue_read_ok && scan.bin_read_ok;
        char cue_line[48];
        char bin_line[48];
        snprintf(cue_line, sizeof(cue_line), "CUE FILES: %d", scan.cue_count);
        snprintf(bin_line, sizeof(bin_line), "BIN FILES: %d", scan.bin_count);

        clear_screen(bg);
        rect(0,0,FB_WIDTH,12,accent);
        rect(48,40,FB_WIDTH-96,FB_HEIGHT-80,panel);

        draw_text(86,70,4,"CRASH RECOMP VITA",text);
        draw_text(88,120,2,"DISC ACCESS TEST",accent);
        draw_text(88,160,2,"PATH: UX0:DATA/CRASHRECOMP/",text);
        draw_text(88,198,2,scan.dir_ok ? "DATA FOLDER: OK" : "DATA FOLDER: ERROR",scan.dir_ok ? ok : bad);
        draw_text(88,232,2,cue_line,scan.cue_count > 0 ? ok : warn);
        draw_text(88,266,2,bin_line,scan.bin_count > 0 ? ok : warn);
        draw_text(88,300,2,pair_ready ? "DISC PAIR: READY" : "DISC PAIR: WAITING",pair_ready ? ok : warn);
        draw_text(88,334,2,readable ? "FILE READ: OK" : "FILE READ: WAITING",readable ? ok : warn);
        draw_text(88,384,2,"TRIANGLE TO RESCAN",text);
        draw_text(88,418,2,"X INPUT TEST",text);
        draw_text(88,452,2,"START TO EXIT",text);

        if (pad.buttons & SCE_CTRL_CROSS) {
            rect(734,200,100,100,accent);
            draw_text(764,230,3,"X",bg);
        }

        int pulse = (int)((frame / 15u) % 12u);
        rect(88,495,12 + pulse*10,8,readable ? ok : accent);

        present(g_framebuffer);
        draw_index ^= 1;
        frame++;

        if (pad.buttons & SCE_CTRL_START) running = 0;
        previous_buttons = pad.buttons;
    }

    free_framebuffers();
    sceKernelExitProcess(0);
    return 0;
}
