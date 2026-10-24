// smrun: runs the GBA cartridge in libmgba, headless, printing its debug
// log and saving screenshots.
//
//     smrun ROM FRAMES [SHOTS OUT_PREFIX]
//
// SHOTS is a comma-separated list of frame numbers (1-based) to save as
// OUT_PREFIX<frame>.png.
//
// With KEYS=FROM:STEP:K1,K2,... in the environment, it holds buttons: from
// frame FROM, K1 for STEP frames, then K2, and so on, round and round. Each
// K is buttons joined by '+' (A, B, SELECT, START, RIGHT, LEFT, UP, DOWN, R,
// L), or '-' for none.
//
// With AFTER=TEXT, KEYS and SHOTS count frames from the first log line
// containing TEXT (frame 1 is the frame after), and PRE_KEYS (in KEYS's
// form, counting from the start) holds buttons before then.
//
// With PROFILE=N SYMS=FILE, it runs the N frames from frame PROFILE_FROM
// (default 1, counted like SHOTS) an instruction at a time, and prints
// where their cycles went, by function (the PROFILE_TOP most, default
// 50): SYMS is `llvm-nm -n sm.elf`. With
// PCS=LO:HI (hex) too, it also prints the cycles and count of each
// instruction with an
// address in [LO, HI), to see where in a function they go.
//
// With TRAP0=1 too, if the CPU goes to address 0 or the cartridge's entry
// (a call through a null pointer, which resets the GBA), it prints the 64
// instructions before, with their functions and LR, and stops.
//
// With DUMP=FRAME:PREFIX, it writes the GBA's memory after that frame
// (counted like SHOTS) to PREFIX.ewram, PREFIX.iwram, .io, .pal, .vram and
// .oam: for tools/snesref.c, and for looking into.
//
// With SHOT_ON=TEXT, for each log line containing TEXT, it saves the
// picture two frames later (time enough for it to be on screen) as
// OUT_PREFIX<label>.png, and the memory as DUMP's does, to OUT_PREFIX<label>:
// the label is the word after TEXT (gba/tour.c holds its frames for this).
//
// With STOP_ON=TEXT, it stops (a few frames after the shot a line may ask
// for) at the first log line containing TEXT, before FRAMES.
//
// With AUDIO=OUT.wav, it records the sound, at 32768 Hz.
//
// With SAVE=FILE.sav, the cartridge's SRAM starts as that file's (which is
// left as it was: the game's saves go to a copy in memory).
//
// Build with tools/mgba/build.sh, against mGBA 0.10.
#include <mgba/flags.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t parse_keys(const char* k, const char* end) {
    static const char* names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN",
                                  "R", "L"};
    uint32_t keys = 0;
    while (k < end) {
        const char* plus = memchr(k, '+', end - k);
        const char* stop = plus ? plus : end;
        for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
            if (strlen(names[i]) == (size_t) (stop - k) && !strncmp(names[i], k, stop - k))
                keys |= 1u << i;
        }
        k = plus ? plus + 1 : end;
    }
    return keys;
}

static int current_frame;
static const char* after_text;
static int after_frame;  // when after_text appeared, or 0
static const char* shot_text;
static int shot_frame;  // when to save SHOT_ON's next, or 0
static char shot_label[64];
static const char* stop_text;
static int stop_frame;  // when to stop, or 0

static void logger_fn(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args) {
    (void) logger;
    const char* name = mLogCategoryName(category);
    if (level & (mLOG_FATAL | mLOG_ERROR) || (name && !strcmp(name, "GBA Debug"))) {
        char line[1024];
        vsnprintf(line, sizeof line, format, args);
        printf("[%d] %s\n", current_frame, line);
        fflush(stdout);
        if (after_text && !after_frame && strstr(line, after_text))
            after_frame = current_frame;
        if (stop_text && !stop_frame && strstr(line, stop_text))
            stop_frame = current_frame + 8;
        const char* shot = shot_text ? strstr(line, shot_text) : NULL;
        if (shot) {
            shot_frame = current_frame + 2;
            sscanf(shot + strlen(shot_text), " %63s", shot_label);
        }
    }
}

struct Keys {
    int from, step, n;
    uint32_t steps[4096];
};

static struct Keys parse_schedule(const char* spec) {
    struct Keys k = {.from = -1, .step = 1};
    if (!spec)
        return k;
    k.from = atoi(spec);
    const char* colon = strchr(spec, ':');
    k.step = colon ? atoi(colon + 1) : 1;
    const char* list = colon ? strchr(colon + 1, ':') : NULL;
    for (const char* p = list ? list + 1 : ""; *p && k.n < 4096;) {
        const char* comma = strchr(p, ',');
        const char* end = comma ? comma : p + strlen(p);
        k.steps[k.n++] = parse_keys(p, end);
        p = comma ? comma + 1 : end;
    }
    return k;
}

static uint32_t schedule_keys(const struct Keys* k, int frame) {
    if (!k->n || frame < k->from)
        return 0;
    return k->steps[((frame - k->from) / k->step) % k->n];
}

struct Sym {
    uint32_t addr;
    char name[96];
    unsigned long long cycles;
};
static uint32_t pcs_lo, pcs_hi;
static unsigned long long *pcs_cycles, *pcs_count;
static struct Sym* syms;
static int nsyms;

static void load_syms(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't read %s\n", path);
        exit(1);
    }
    char line[512];
    int cap = 4096;
    // Below every symbol: the BIOS, as in VBlankIntrWait's halt.
    syms = realloc(syms, cap * sizeof *syms);
    syms[nsyms].addr = 0;
    snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "(bios)");
    syms[nsyms++].cycles = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned addr;
        char type, name[256];
        if (sscanf(line, "%x %c %255s", &addr, &type, name) != 3 || (type != 't' && type != 'T'))
            continue;
        if (nsyms == cap)
            syms = realloc(syms, (cap = cap ? cap * 2 : 4096) * sizeof *syms);
        syms[nsyms].addr = addr & ~1u;
        snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "%s", name);
        syms[nsyms].cycles = 0;
        nsyms++;
    }
    fclose(f);
}

static struct Sym* find_sym(uint32_t pc) {
    int lo = 0, hi = nsyms - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (syms[mid].addr <= pc)
            lo = mid;
        else
            hi = mid - 1;
    }
    return &syms[lo];
}

static int by_cycles(const void* a, const void* b) {
    const struct Sym *x = a, *y = b;
    return x->cycles < y->cycles ? 1 : x->cycles > y->cycles ? -1 : 0;
}

// Runs a frame an instruction at a time, adding each one's cycles to its
// function's.
static unsigned long long profile_frame(struct mCore* core) {
    struct ARMCore* cpu = core->cpu;
    uint32_t start_frame = core->frameCounter(core);
    uint64_t last = mTimingGlobalTime(core->timing), start = last;
    // The last instructions run, for TRAP0.
    static uint32_t ring[64], ring_lr[64];
    static unsigned ring_at;
    while (core->frameCounter(core) == start_frame) {
        uint32_t pc = cpu->gprs[15] - (cpu->executionMode == MODE_ARM ? 4 : 2);
        if (getenv("TRAP0") && (pc == 0 || pc == 0x08000000)) {
            printf("trap: pc 0 at frame %u\n", core->frameCounter(core));
            for (unsigned k = 0; k < 64; k++) {
                uint32_t a = ring[(ring_at + k) % 64];
                printf("trap: %08x %s (lr %08x %s)\n", a, find_sym(a)->name,
                       ring_lr[(ring_at + k) % 64], find_sym(ring_lr[(ring_at + k) % 64])->name);
            }
            exit(1);
        }
        ring[ring_at % 64] = pc, ring_lr[ring_at % 64] = cpu->gprs[14], ring_at++;
        core->step(core);
        uint64_t now = mTimingGlobalTime(core->timing);
        find_sym(pc)->cycles += now - last;
        if (pcs_cycles && pc >= pcs_lo && pc < pcs_hi)
            pcs_cycles[(pc - pcs_lo) / 2] += now - last, pcs_count[(pc - pcs_lo) / 2]++;
        last = now;
    }
    return last - start;
}

static void dump_memory(struct mCore* core, const char* prefix) {
    static const struct { const char* ext; uint32_t base, size; } kRegions[] = {
        {"ewram", 0x02000000, 0x40000},
        {"iwram", 0x03000000, 0x8000},
        {"io", 0x04000000, 0x400},
        {"pal", 0x05000000, 0x400},
        {"vram", 0x06000000, 0x18000},
        {"oam", 0x07000000, 0x400},
    };
    for (unsigned r = 0; r < sizeof kRegions / sizeof *kRegions; r++) {
        char path[512];
        snprintf(path, sizeof path, "%s.%s", prefix, kRegions[r].ext);
        FILE* f = fopen(path, "wb");
        for (uint32_t a = 0; a < kRegions[r].size; a++)
            fputc(core->rawRead8(core, kRegions[r].base + a, -1), f);
        fclose(f);
    }
}

static FILE* audio_out;
static uint32_t audio_bytes;

static void write_wav_header(FILE* f, uint32_t bytes) {
    uint32_t rate = 32768;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    uint32_t v = 36 + bytes;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt[4] = {16, 1 | 2 << 16, rate, rate * 4};
    fwrite(fmt, 4, 4, f);
    uint16_t align[2] = {4, 16};
    fwrite(align, 2, 2, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
}

static void record_audio(struct mCore* core) {
    blip_t* left = core->getAudioChannel(core, 0);
    blip_t* right = core->getAudioChannel(core, 1);
    int available = blip_samples_avail(left);
    static short buf[2 * 8192];
    while (available > 0) {
        int n = available > 8192 ? 8192 : available;
        blip_read_samples(left, buf, n, 1);
        blip_read_samples(right, buf + 1, n, 1);
        fwrite(buf, 4, n, audio_out);
        audio_bytes += 4 * n;
        available -= n;
    }
}

static void put32(unsigned char* p, uint32_t v) {
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static void chunk(FILE* f, const char* type, const unsigned char* data, uint32_t len) {
    unsigned char hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    // (zlib's crc32 of a null buffer is 0, not the CRC so far: IEND has no data.)
    uint32_t crc = crc32(crc32(0, (const unsigned char*) type, 4), data ? data : (const unsigned char*) "", len);
    unsigned char c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

static void write_png(const char* path, const color_t* pixels, unsigned w, unsigned h, unsigned stride) {
    size_t raw_len = (size_t) h * (w * 3 + 1);
    unsigned char* raw = malloc(raw_len);
    for (unsigned y = 0; y < h; ++y) {
        unsigned char* row = raw + y * (w * 3 + 1);
        row[0] = 0;
        for (unsigned x = 0; x < w; ++x) {
            uint32_t c = pixels[y * stride + x];
            row[1 + x * 3] = c & 0xff;
            row[2 + x * 3] = (c >> 8) & 0xff;
            row[3 + x * 3] = (c >> 16) & 0xff;
        }
    }
    uLongf zlen = compressBound(raw_len);
    unsigned char* z = malloc(zlen);
    compress(z, &zlen, raw, raw_len);
    FILE* f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s ROM FRAMES [SHOTS OUT_PREFIX]\n", argv[0]);
        return 2;
    }
    static struct mLogger logger = {.log = logger_fn};
    mLogSetDefaultLogger(&logger);
    struct mCore* core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) {
        fprintf(stderr, "%s: can't open %s\n", argv[0], argv[1]);
        return 1;
    }
    mCoreInitConfig(core, NULL);
    unsigned width, height;
    core->desiredVideoDimensions(core, &width, &height);
    color_t* pixels = calloc(width * height, sizeof(color_t));
    core->setVideoBuffer(core, pixels, width);
    if (!mCoreLoadFile(core, argv[1])) {
        fprintf(stderr, "%s: can't load %s\n", argv[0], argv[1]);
        return 1;
    }
    if (getenv("SAVE")) {
        struct VFile* vf = VFileOpen(getenv("SAVE"), O_RDONLY);
        if (!vf || !core->loadTemporarySave(core, vf)) {
            fprintf(stderr, "%s: can't load %s\n", argv[0], getenv("SAVE"));
            return 1;
        }
    }
    core->reset(core);

    struct Keys pre = parse_schedule(getenv("PRE_KEYS"));
    struct Keys keys = parse_schedule(getenv("KEYS"));
    after_text = getenv("AFTER");
    shot_text = getenv("SHOT_ON");
    stop_text = getenv("STOP_ON");
    if (getenv("AUDIO")) {
        audio_out = fopen(getenv("AUDIO"), "wb");
        write_wav_header(audio_out, 0);
        core->setAudioBufferSize(core, 8192);
        blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), 32768);
        blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), 32768);
    }
    int profile_n = getenv("PROFILE") ? atoi(getenv("PROFILE")) : 0;
    int profile_from = getenv("PROFILE_FROM") ? atoi(getenv("PROFILE_FROM")) : 1;
    unsigned long long profile_total = 0;
    if (profile_n)
        load_syms(getenv("SYMS"));
    if (getenv("PCS")) {
        pcs_lo = strtoul(getenv("PCS"), NULL, 16) & ~1u;
        const char* colon = strchr(getenv("PCS"), ':');
        pcs_hi = colon ? strtoul(colon + 1, NULL, 16) : pcs_lo;
        pcs_cycles = calloc((pcs_hi - pcs_lo) / 2 + 1, sizeof *pcs_cycles);
        pcs_count = calloc((pcs_hi - pcs_lo) / 2 + 1, sizeof *pcs_count);
    }
    int frames = atoi(argv[2]);
    const char* shots = argc > 3 ? argv[3] : "";
    const char* prefix = argc > 4 ? argv[4] : "shot";
    for (int i = 1; i <= frames; ++i) {
        current_frame = i;
        // Frames counted from AFTER's line, when there is one.
        int t = !after_text ? i : after_frame ? i - after_frame : 0;
        if (after_text && !after_frame)
            core->setKeys(core, schedule_keys(&pre, i));
        else
            core->setKeys(core, schedule_keys(&keys, t));
        bool profiling = profile_n && t >= profile_from && t < profile_from + profile_n &&
                         (!after_text || after_frame);
        if (profiling)
            profile_total += profile_frame(core);
        else
            core->runFrame(core);
        if (audio_out)
            record_audio(core);
        if (profile_n && t == profile_from + profile_n - 1 && profile_total) {
            qsort(syms, nsyms, sizeof *syms, by_cycles);
            printf("profile: %llu cycles over %d frames\n", profile_total, profile_n);
            for (int k = 0; k < (getenv("PROFILE_TOP") ? atoi(getenv("PROFILE_TOP")) : 50) && syms[k].cycles; k++)
                printf("%5.1f%% %10llu  %s\n", 100.0 * syms[k].cycles / profile_total, syms[k].cycles,
                       syms[k].name);
            for (uint32_t a = pcs_lo; pcs_cycles && a < pcs_hi; a += 2) {
                if (pcs_cycles[(a - pcs_lo) / 2])
                    printf("pc %08x %10llu %8llu\n", a, pcs_cycles[(a - pcs_lo) / 2],
                           pcs_count[(a - pcs_lo) / 2]);
            }
            break;
        }
        const char* dump = getenv("DUMP");
        if (dump && t > 0 && atoi(dump) == t && strchr(dump, ':'))
            dump_memory(core, strchr(dump, ':') + 1);
        if (shot_frame == i) {
            char path[512];
            snprintf(path, sizeof path, "%s%s", prefix, shot_label);
            dump_memory(core, path);
            snprintf(path, sizeof path, "%s%s.png", prefix, shot_label);
            write_png(path, pixels, width, height, width);
        }
        if (stop_frame == i)
            break;
        for (const char* s = shots; *s && t > 0;) {
            if (atoi(s) == t) {
                char path[512];
                snprintf(path, sizeof path, "%s%d.png", prefix, t);
                write_png(path, pixels, width, height, width);
            }
            const char* comma = strchr(s, ',');
            s = comma ? comma + 1 : s + strlen(s);
        }
    }
    if (audio_out) {
        write_wav_header(audio_out, audio_bytes);
        fclose(audio_out);
    }
    core->deinit(core);
    return 0;
}
