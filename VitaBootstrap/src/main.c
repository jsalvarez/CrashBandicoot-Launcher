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
#define RAW_SECTOR_SIZE 2352
#define MODE2_USER_OFFSET 24
#define ISO_SECTOR_SIZE 2048
#define EXPECTED_BOOT_ID "SCUS_949.00"

static uint32_t *g_framebuffer;
static uint32_t *g_framebuffers[2];
static SceUID g_fb_blocks[2] = {-1, -1};

typedef struct {
    uint32_t extent;
    uint32_t size;
    uint8_t flags;
    char name[128];
} IsoEntry;

typedef struct {
    int dir_ok;
    int cue_count;
    int cue_read_ok;
    int cue_parse_ok;
    int bin_read_ok;
    int iso_ok;
    int system_cnf_ok;
    int boot_entry_ok;
    int expected_game_ok;
    int psx_exe_ok;
    char cue_name[256];
    char bin_name[256];
    char boot_name[128];
    uint32_t data_track_sector;
    uint32_t exe_pc;
    uint32_t exe_gp;
    uint32_t exe_load;
    uint32_t exe_size;
} DiscInfo;

static inline uint32_t abgr(uint8_t a, uint8_t b, uint8_t g, uint8_t r) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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
    {' ', {0,0,0,0,0,0,0}}, {'-', {0,0,0,31,0,0,0}}, {'.', {0,0,0,0,0,6,6}}, {':', {0,6,6,0,6,6,0}}, {'/', {1,2,4,8,16,0,0}}, {'_', {0,0,0,0,0,0,31}},
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
            if (rows[gy] & (1u << (4-gx))) rect(x + gx*scale, y + gy*scale, scale, scale, color);
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

static int text_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int text_starts_ci(const char *text, const char *prefix) {
    while (*prefix) {
        if (!*text || ascii_lower(*text) != ascii_lower(*prefix)) return 0;
        ++text; ++prefix;
    }
    return 1;
}

static int ends_with_ci(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len) return 0;
    return text_eq_ci(name + name_len - suffix_len, suffix);
}

static int file_exists_readable(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    uint8_t probe[16];
    SceSSize got = sceIoRead(fd, probe, sizeof(probe));
    sceIoClose(fd);
    return got > 0;
}

static int read_text_file(const char *path, char *out, int capacity) {
    if (capacity < 2) return -1;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    SceSSize got = sceIoRead(fd, out, capacity - 1);
    sceIoClose(fd);
    if (got <= 0) return -1;
    out[got] = '\0';
    return (int)got;
}

static int parse_msf(const char *s, uint32_t *sector) {
    unsigned mm = 0, ss = 0, ff = 0;
    if (sscanf(s, "%u:%u:%u", &mm, &ss, &ff) != 3) return 0;
    if (ss >= 60 || ff >= 75) return 0;
    *sector = (uint32_t)(mm * 60u * 75u + ss * 75u + ff);
    return 1;
}

static int parse_cue(const char *cue_path, char *bin_name, size_t bin_cap, uint32_t *track_sector) {
    char text[8192];
    if (read_text_file(cue_path, text, sizeof(text)) < 0) return 0;

    char current_file[256] = {0};
    int in_data_track = 0;
    char *line = text;

    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        while (*line == ' ' || *line == '\t' || *line == '\r') ++line;

        if (text_starts_ci(line, "FILE ")) {
            const char *p = line + 5;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '"') {
                ++p;
                const char *q = strchr(p, '"');
                if (q) {
                    size_t n = (size_t)(q - p);
                    if (n >= sizeof(current_file)) n = sizeof(current_file) - 1;
                    memcpy(current_file, p, n);
                    current_file[n] = '\0';
                }
            } else {
                size_t n = 0;
                while (p[n] && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(current_file)) ++n;
                memcpy(current_file, p, n);
                current_file[n] = '\0';
            }
            in_data_track = 0;
        } else if (text_starts_ci(line, "TRACK ")) {
            in_data_track = strstr(line, "MODE2/2352") != NULL || strstr(line, "mode2/2352") != NULL;
        } else if (in_data_track && text_starts_ci(line, "INDEX 01 ")) {
            uint32_t sec = 0;
            if (!current_file[0] || !parse_msf(line + 9, &sec)) return 0;
            snprintf(bin_name, bin_cap, "%s", current_file);
            *track_sector = sec;
            return 1;
        }

        if (!next) break;
        line = next + 1;
    }
    return 0;
}

static int read_mode2_sector(SceUID fd, uint32_t sector, uint8_t out[ISO_SECTOR_SIZE]) {
    SceOff offset = (SceOff)sector * RAW_SECTOR_SIZE + MODE2_USER_OFFSET;
    if (sceIoLseek(fd, offset, SCE_SEEK_SET) < 0) return 0;
    return sceIoRead(fd, out, ISO_SECTOR_SIZE) == ISO_SECTOR_SIZE;
}

static void iso_clean_name(const uint8_t *raw, int raw_len, char *out, size_t out_cap) {
    size_t n = 0;
    for (int i = 0; i < raw_len && n + 1 < out_cap; ++i) {
        char c = (char)raw[i];
        if (c == ';') break;
        out[n++] = c;
    }
    while (n > 0 && out[n-1] == '.') --n;
    out[n] = '\0';
}

static int iso_find_root_entry(SceUID fd, uint32_t track_sector, uint32_t root_extent, uint32_t root_size,
                               const char *wanted, IsoEntry *result) {
    uint8_t sector[ISO_SECTOR_SIZE];
    uint32_t sectors = (root_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    for (uint32_t s = 0; s < sectors; ++s) {
        if (!read_mode2_sector(fd, track_sector + root_extent + s, sector)) return 0;
        uint32_t pos = 0;
        while (pos < ISO_SECTOR_SIZE) {
            uint8_t len = sector[pos];
            if (len == 0) break;
            if (pos + len > ISO_SECTOR_SIZE || len < 34) break;

            const uint8_t *rec = sector + pos;
            int name_len = rec[32];
            if (33 + name_len <= len && name_len > 0 && rec[33] != 0 && rec[33] != 1) {
                char clean[128];
                iso_clean_name(rec + 33, name_len, clean, sizeof(clean));
                if (text_eq_ci(clean, wanted)) {
                    memset(result, 0, sizeof(*result));
                    result->extent = le32(rec + 2);
                    result->size = le32(rec + 10);
                    result->flags = rec[25];
                    snprintf(result->name, sizeof(result->name), "%s", clean);
                    return 1;
                }
            }
            pos += len;
        }
    }
    return 0;
}

static int iso_read_file_prefix(SceUID fd, uint32_t track_sector, const IsoEntry *entry,
                                uint8_t *out, uint32_t max_bytes) {
    uint32_t wanted = entry->size < max_bytes ? entry->size : max_bytes;
    uint32_t copied = 0;
    uint8_t sector[ISO_SECTOR_SIZE];

    while (copied < wanted) {
        uint32_t sec_index = copied / ISO_SECTOR_SIZE;
        if (!read_mode2_sector(fd, track_sector + entry->extent + sec_index, sector)) return 0;
        uint32_t remain = wanted - copied;
        uint32_t chunk = remain < ISO_SECTOR_SIZE ? remain : ISO_SECTOR_SIZE;
        memcpy(out + copied, sector, chunk);
        copied += chunk;
    }
    return (int)copied;
}

static int parse_boot_name(const char *system_cnf, char *out, size_t out_cap) {
    const char *p = system_cnf;
    while (*p) {
        if (text_starts_ci(p, "cdrom:")) {
            p += 6;
            while (*p == '\\' || *p == '/') ++p;
            const char *base = p;
            for (const char *q = p; *q && *q != '\r' && *q != '\n' && *q != ' ' && *q != '\t'; ++q) {
                if (*q == '\\' || *q == '/') base = q + 1;
            }
            size_t n = 0;
            while (base[n] && base[n] != ';' && base[n] != '\r' && base[n] != '\n' &&
                   base[n] != ' ' && base[n] != '\t' && n + 1 < out_cap) {
                out[n] = base[n];
                ++n;
            }
            out[n] = '\0';
            return n > 0;
        }
        ++p;
    }
    return 0;
}

static void inspect_disc(DiscInfo *info) {
    memset(info, 0, sizeof(*info));

    /* IMPORTANT: final design is read-only. We never create DATA_DIR. */
    SceUID dir = sceIoDopen(DATA_DIR);
    if (dir < 0) return;
    info->dir_ok = 1;

    for (;;) {
        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        int rc = sceIoDread(dir, &entry);
        if (rc <= 0) break;
        if (!SCE_S_ISREG(entry.d_stat.st_mode)) continue;
        if (ends_with_ci(entry.d_name, ".cue")) {
            info->cue_count++;
            if (!info->cue_name[0]) snprintf(info->cue_name, sizeof(info->cue_name), "%s", entry.d_name);
        }
    }
    sceIoDclose(dir);
    if (!info->cue_name[0]) return;

    char cue_path[512];
    snprintf(cue_path, sizeof(cue_path), "%s/%s", DATA_DIR, info->cue_name);
    info->cue_read_ok = file_exists_readable(cue_path);
    if (!info->cue_read_ok) return;

    if (!parse_cue(cue_path, info->bin_name, sizeof(info->bin_name), &info->data_track_sector)) return;
    info->cue_parse_ok = 1;

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/%s", DATA_DIR, info->bin_name);
    info->bin_read_ok = file_exists_readable(bin_path);
    if (!info->bin_read_ok) return;

    SceUID fd = sceIoOpen(bin_path, SCE_O_RDONLY, 0);
    if (fd < 0) return;

    uint8_t pvd[ISO_SECTOR_SIZE];
    if (!read_mode2_sector(fd, info->data_track_sector + 16, pvd) ||
        pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        sceIoClose(fd);
        return;
    }
    info->iso_ok = 1;

    const uint8_t *root = pvd + 156;
    uint32_t root_extent = le32(root + 2);
    uint32_t root_size = le32(root + 10);

    IsoEntry sys;
    if (!iso_find_root_entry(fd, info->data_track_sector, root_extent, root_size, "SYSTEM.CNF", &sys)) {
        sceIoClose(fd);
        return;
    }

    uint8_t system_buf[1025];
    memset(system_buf, 0, sizeof(system_buf));
    int system_len = iso_read_file_prefix(fd, info->data_track_sector, &sys, system_buf, 1024);
    if (system_len <= 0) {
        sceIoClose(fd);
        return;
    }
    system_buf[system_len] = '\0';
    info->system_cnf_ok = 1;

    if (!parse_boot_name((const char *)system_buf, info->boot_name, sizeof(info->boot_name))) {
        sceIoClose(fd);
        return;
    }
    info->boot_entry_ok = 1;
    info->expected_game_ok = text_eq_ci(info->boot_name, EXPECTED_BOOT_ID);

    IsoEntry exe;
    if (!iso_find_root_entry(fd, info->data_track_sector, root_extent, root_size, info->boot_name, &exe)) {
        sceIoClose(fd);
        return;
    }

    uint8_t exe_header[ISO_SECTOR_SIZE];
    if (iso_read_file_prefix(fd, info->data_track_sector, &exe, exe_header, sizeof(exe_header)) < 0x800 ||
        memcmp(exe_header, "PS-X EXE", 8) != 0) {
        sceIoClose(fd);
        return;
    }

    info->psx_exe_ok = 1;
    info->exe_pc = le32(exe_header + 0x10);
    info->exe_gp = le32(exe_header + 0x14);
    info->exe_load = le32(exe_header + 0x18);
    info->exe_size = le32(exe_header + 0x1C);
    sceIoClose(fd);
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

    DiscInfo disc;
    inspect_disc(&disc);

    int running = 1;
    int draw_index = 0;
    uint32_t frame = 0;
    uint32_t previous_buttons = 0;

    while (running) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_TRIANGLE) && !(previous_buttons & SCE_CTRL_TRIANGLE)) inspect_disc(&disc);

        g_framebuffer = g_framebuffers[draw_index];
        uint32_t bg = abgr(255,22,19,18);
        uint32_t panel = abgr(255,39,35,32);
        uint32_t text = abgr(255,235,235,235);
        uint32_t dim = abgr(255,160,160,160);
        uint32_t accent = abgr(255,70,163,255);
        uint32_t ok = abgr(255,90,210,120);
        uint32_t warn = abgr(255,90,185,245);
        uint32_t bad = abgr(255,85,85,235);

        clear_screen(bg);
        rect(0,0,FB_WIDTH,12,accent);
        rect(38,30,FB_WIDTH-76,FB_HEIGHT-60,panel);

        draw_text(70,52,3,"CRASH RECOMP VITA",text);
        draw_text(70,84,2,"PRE-FINAL NATIVE HOST",accent);
        draw_text(70,112,1,"TARGET: 960X544 16:9 / 60 HZ",dim);
        draw_text(70,132,1,"DATA PATH IS READ-ONLY - FOLDER IS NEVER CREATED",dim);

        draw_text(70,164,2,disc.dir_ok ? "DATA FOLDER: OK" : "DATA FOLDER: MISSING",disc.dir_ok ? ok : bad);
        draw_text(70,190,2,disc.cue_count > 0 ? "CUE: FOUND" : "CUE: MISSING",disc.cue_count > 0 ? ok : warn);
        draw_text(70,216,2,disc.cue_parse_ok ? "CUE PARSE: OK" : "CUE PARSE: WAITING",disc.cue_parse_ok ? ok : warn);
        draw_text(70,242,2,disc.bin_read_ok ? "BIN TRACK: OK" : "BIN TRACK: WAITING",disc.bin_read_ok ? ok : warn);
        draw_text(70,268,2,disc.iso_ok ? "ISO9660: OK" : "ISO9660: WAITING",disc.iso_ok ? ok : warn);
        draw_text(70,294,2,disc.system_cnf_ok ? "SYSTEM.CNF: OK" : "SYSTEM.CNF: WAITING",disc.system_cnf_ok ? ok : warn);

        char boot_line[64];
        snprintf(boot_line, sizeof(boot_line), "BOOT: %s", disc.boot_entry_ok ? disc.boot_name : "WAITING");
        draw_text(70,320,2,boot_line,disc.expected_game_ok ? ok : (disc.boot_entry_ok ? bad : warn));
        draw_text(70,346,2,disc.psx_exe_ok ? "PS-X EXE: VALID" : "PS-X EXE: WAITING",disc.psx_exe_ok ? ok : warn);

        if (disc.psx_exe_ok) {
            char pc_line[64], load_line[64], size_line[64];
            snprintf(pc_line, sizeof(pc_line), "ENTRY PC: %08X", (unsigned)disc.exe_pc);
            snprintf(load_line, sizeof(load_line), "LOAD ADDR: %08X", (unsigned)disc.exe_load);
            snprintf(size_line, sizeof(size_line), "TEXT SIZE: %u BYTES", (unsigned)disc.exe_size);
            draw_text(70,378,1,pc_line,text);
            draw_text(70,398,1,load_line,text);
            draw_text(70,418,1,size_line,text);
        }

        int host_ready = disc.psx_exe_ok && disc.expected_game_ok;
        draw_text(70,448,2,host_ready ? "DISC HOST: READY FOR GAME CODE" : "DISC HOST: NOT READY",host_ready ? ok : warn);
        draw_text(70,476,1,"TRIANGLE RESCAN   START EXIT",dim);

        int pulse = (int)((frame / 15u) % 12u);
        rect(70,505,12 + pulse*9,6,host_ready ? ok : accent);

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
