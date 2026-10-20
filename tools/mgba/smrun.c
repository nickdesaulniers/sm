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
// Build with tools/mgba/build.sh, against mGBA 0.10.
#include <mgba/flags.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
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

static void logger_fn(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args) {
    (void) logger;
    const char* name = mLogCategoryName(category);
    if (level & (mLOG_FATAL | mLOG_ERROR) || (name && !strcmp(name, "GBA Debug"))) {
        printf("[%d] ", current_frame);
        vprintf(format, args);
        putchar('\n');
        fflush(stdout);
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
    uint32_t crc = crc32(crc32(0, (const unsigned char*) type, 4), data, len);
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
    core->reset(core);

    int keys_from = -1, keys_step = 1, nkeys = 0;
    uint32_t key_steps[256];
    if (getenv("KEYS")) {
        const char* spec = getenv("KEYS");
        keys_from = atoi(spec);
        const char* colon = strchr(spec, ':');
        keys_step = colon ? atoi(colon + 1) : 1;
        const char* list = colon ? strchr(colon + 1, ':') : NULL;
        for (const char* k = list ? list + 1 : ""; *k && nkeys < 256;) {
            const char* comma = strchr(k, ',');
            const char* end = comma ? comma : k + strlen(k);
            key_steps[nkeys++] = parse_keys(k, end);
            k = comma ? comma + 1 : end;
        }
    }
    int frames = atoi(argv[2]);
    const char* shots = argc > 3 ? argv[3] : "";
    const char* prefix = argc > 4 ? argv[4] : "shot";
    for (int i = 1; i <= frames; ++i) {
        current_frame = i;
        if (nkeys && i >= keys_from)
            core->setKeys(core, key_steps[((i - keys_from) / keys_step) % nkeys]);
        core->runFrame(core);
        for (const char* s = shots; *s;) {
            if (atoi(s) == i) {
                char path[512];
                snprintf(path, sizeof path, "%s%d.png", prefix, i);
                write_png(path, pixels, width, height, width);
            }
            const char* comma = strchr(s, ',');
            s = comma ? comma + 1 : s + strlen(s);
        }
    }
    core->deinit(core);
    return 0;
}
