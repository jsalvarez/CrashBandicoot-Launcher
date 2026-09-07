#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdint.h>
#include <string.h>

#define FB_WIDTH 960
#define FB_HEIGHT 544
#define FB_STRIDE 960
#define FB_BLOCK_SIZE (2 * 1024 * 1024)

static uint32_t *g_framebuffer;
static uint32_t *g_framebuffers[2];
static SceUID g_fb_blocks[2] = {-1, -1};

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
    {' ', {0,0,0,0,0,0,0}}, {'-', {0,0,0,31,0,0,0}}, {'.', {0,0,0,0,0,6,6}}, {':', {0,6,6,0,6,6,0}},
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

    int running = 1;
    int draw_index = 0;
    uint32_t frame = 0;

    while (running) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        /* Draw only into the buffer that is not currently being scanned out. */
        g_framebuffer = g_framebuffers[draw_index];

        uint32_t bg = abgr(255,22,19,18);
        uint32_t panel = abgr(255,39,35,32);
        uint32_t text = abgr(255,235,235,235);
        uint32_t accent = abgr(255,70,163,255);
        uint32_t ok = abgr(255,90,210,120);

        clear_screen(bg);
        rect(0,0,FB_WIDTH,12,accent);
        rect(48,54,FB_WIDTH-96,FB_HEIGHT-108,panel);

        draw_text(86,92,4,"CRASH RECOMP VITA",text);
        draw_text(88,145,2,"NATIVE VITA BOOTSTRAP",accent);
        draw_text(88,195,2,"DISPLAY: OK",ok);
        draw_text(88,229,2,"INPUT:   OK",ok);
        draw_text(88,263,2,"CDRAM:   OK",ok);
        draw_text(88,297,2,"DOUBLE BUFFER: OK",ok);
        draw_text(88,348,2,"RUNTIME: WAITING FOR RECOMPONE",text);
        draw_text(88,397,2,"PRESS X TO FLASH INPUT",text);
        draw_text(88,430,2,"PRESS START TO EXIT",text);

        if (pad.buttons & SCE_CTRL_CROSS) {
            rect(712,204,110,110,accent);
            draw_text(739,240,3,"X",bg);
        }

        int pulse = (int)((frame / 15u) % 12u);
        rect(88,485,12 + pulse*10,8,accent);

        present(g_framebuffer);
        draw_index ^= 1;
        frame++;

        if (pad.buttons & SCE_CTRL_START) running = 0;
    }

    free_framebuffers();
    sceKernelExitProcess(0);
    return 0;
}
