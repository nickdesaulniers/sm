// See audio.h.
//
// The SNES's sound driver runs on its sound CPU, with 64 KB of RAM, and
// plays through its DSP: eight voices of BRR samples, with envelopes,
// echo, noise, at 32 kHz. Here, the driver (src/spc_player.c) runs on
// the GBA's CPU, at its timer's 500 Hz. The GBA has no room for the RAM:
// all the driver reads was uploaded from the ROM, so it reads the ROM
// (SpcRam_Read). In place of the DSP, the driver's writes to its registers
// set up the GBA's mixer: its voices play the samples make_cartridge.py
// decoded, at 10512 Hz, with the DSP's envelopes. No echo yet, nor pitch
// modulation, nor interpolation.
#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "cart_data.h"
#include "gba_hw.h"
#include "sm_rtl.h"
#include "spc_player.h"

// In IWRAM, with the DSP's registers and envelopes: the driver works on
// them 500 times a second.
static SpcPlayer player IWRAM_BSS;

// The APU's RAM: the blocks of the ROM the game uploaded to it, newest
// last.
typedef struct AramBlock {
  uint16_t dest, length;
  const uint8_t *src;
} AramBlock;
enum { kMaxAramBlocks = 32 };
static AramBlock aram[kMaxAramBlocks];
static int aram_count;

static void AddAramBlock(uint16_t dest, uint16_t length, const uint8_t *src) {
  // Older blocks this one covers are gone.
  int n = 0;
  for (int i = 0; i < aram_count; i++) {
    if (aram[i].dest < dest || aram[i].dest + aram[i].length > dest + length)
      aram[n++] = aram[i];
  }
  if (n == kMaxAramBlocks) {
    memmove(aram, aram + 1, sizeof(aram) - sizeof(aram[0]));
    n--;
  }
  aram[n++] = (AramBlock){dest, length, src};
  aram_count = n;
}

static const AramBlock *FindAramBlock(uint16_t addr) {
  for (int i = aram_count - 1; i >= 0; i--) {
    if ((uint16_t)(addr - aram[i].dest) < aram[i].length)
      return &aram[i];
  }
  return NULL;
}

void SpcRam_Upload(const uint8 *data) {
  for (;;) {
    uint16_t length = data[0] | data[1] << 8;
    if (!length)
      return;
    AddAramBlock(data[2] | data[3] << 8, length, data + 4);
    data += 4 + length;
  }
}

uint8 SpcRam_Read(uint16 addr) {
  const AramBlock *b = FindAramBlock(addr);
  return b ? b->src[(uint16_t)(addr - b->dest)] : 0;
}

static uint16_t SpcRam_Read16(uint16_t addr) {
  return SpcRam_Read(addr) | SpcRam_Read(addr + 1) << 8;
}

// The DSP: its registers, and each voice's envelope.
static uint8_t dsp[128] IWRAM_BSS;

enum { kAttack, kDecay, kSustain, kGain, kRelease };
typedef struct Envelope {
  uint8_t state;
  uint16_t level;  // 0-0x7FF
  uint16_t counter;
} Envelope;
static Envelope envelopes[8] IWRAM_BSS;
// In 8.8 fixed point: the DSP's samples (32 kHz) since the envelopes ran.
static uint32_t envelope_time;

// The DSP's samples between envelope steps, by rate.
static const uint16_t kRates[32] = {
  0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
  64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

static const AudioSample *FindSample(uint32_t rom_offset) {
  const uint32_t *audio = (const uint32_t *)((const uint8_t *)&__cart_data + __cart_data.audio_offset);
  const AudioSample *samples = (const AudioSample *)(audio + 1);
  int lo = 0, hi = (int)audio[0] - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (samples[mid].rom_offset == rom_offset)
      return &samples[mid];
    if (samples[mid].rom_offset < rom_offset)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

static void KeyOn(int v) {
  AudioVoice *voice = &g_audio_voices[v];
  Envelope *e = &envelopes[v];
  uint16_t entry = (dsp[0x5d] << 8) + dsp[v * 16 + 4] * 4;
  uint16_t start = SpcRam_Read16(entry), loop = SpcRam_Read16(entry + 2);
  const AramBlock *b = FindAramBlock(start);
  const AudioSample *s = b ? FindSample(b->src + (uint16_t)(start - b->dest) - snes_rom) : NULL;
  voice->noise = (dsp[0x3d] >> v) & 1;
  if (!s && !voice->noise) {
    printf("audio: no sample at 0x%04x (srcn %u)\n", start, dsp[v * 16 + 4]);
    voice->on = false;
    return;
  }
  if (s) {
    voice->pcm = (const int8_t *)&__cart_data + __cart_data.audio_offset + s->pcm_offset;
    voice->end = s->length << 16;
    // BRR is 16 samples in 9 bytes.
    uint32_t loop_start = (uint16_t)(loop - start) / 9 * 16;
    voice->loop_len = s->loops && loop_start < s->length ? (s->length - loop_start) << 16 : 0;
  }
  voice->pos = 0;
  voice->on = true;
  e->level = 0;
  e->counter = 0;
  e->state = (dsp[v * 16 + 5] & 0x80) ? kAttack : kGain;
}

// ARM code in IWRAM, as the driver writes registers 500 times a second.
__attribute__((target("arm"), section(".iwram.text")))
void Audio_DspWrite(uint8 reg, uint8 value) {
  reg &= 0x7f;
  // The driver rewrites most registers unchanged, 500 times a second; KON
  // and KOF act whatever they were.
  if (dsp[reg] == value && reg != 0x4c && reg != 0x5c)
    return;
  dsp[reg] = value;
  int v = reg >> 4;
  if ((reg & 0xe) == 0x2) {
    // 0x1000 is the DSP's 32 kHz: in the mixer's samples, 16.16.
    uint32_t pitch = (dsp[v * 16 + 2] | dsp[v * 16 + 3] << 8) & 0x3fff;
    g_audio_voices[v].step = pitch * (16 * kAudioDspSamples) >> 8;
    return;
  }
  switch (reg) {
  case 0x4c:  // KON
    for (int i = 0; i < 8; i++) {
      if (value & (1 << i))
        KeyOn(i);
    }
    break;
  case 0x5c:  // KOF
    for (int i = 0; i < 8; i++) {
      if (value & (1 << i))
        envelopes[i].state = kRelease;
    }
    break;
  case 0x6c:  // FLG: soft reset, noise rate
    g_audio_noise_period = kRates[value & 0x1f];
    if (value & 0x80) {
      for (int i = 0; i < 8; i++) {
        envelopes[i].state = kRelease;
        envelopes[i].level = 0;
      }
    }
    break;
  }
}

// One step of an envelope, as src/snes/dsp.c's dsp_handleGain.
static void EnvelopeStep(Envelope *e, int v) {
  int level = e->level;
  switch (e->state) {
  case kAttack:
    level += (dsp[v * 16 + 5] & 0xf) == 0xf ? 1024 : 32;
    if (level >= 0x7e0)
      e->state = kDecay;
    if (level > 0x7ff)
      level = 0x7ff;
    break;
  case kDecay:
    level -= ((level - 1) >> 8) + 1;
    if (level < (((dsp[v * 16 + 6] >> 5) + 1) << 8))
      e->state = kSustain;
    break;
  case kSustain:
    level -= ((level - 1) >> 8) + 1;
    break;
  case kGain:
    switch ((dsp[v * 16 + 7] >> 5) & 3) {
    case 0: level = level >= 32 ? level - 32 : 0; break;
    case 1: level -= ((level - 1) >> 8) + 1; break;
    case 2: level = level + 32 > 0x7ff ? 0x7ff : level + 32; break;
    case 3: level += level < 0x600 ? 32 : 8; if (level > 0x7ff) level = 0x7ff; break;
    }
    break;
  }
  e->level = level < 0 ? 0 : level;
}

static unsigned EnvelopeRate(const Envelope *e, int v) {
  const uint8_t *r = &dsp[v * 16];
  switch (e->state) {
  case kAttack: return kRates[(r[5] & 0xf) * 2 + 1];
  case kDecay: return kRates[((r[5] >> 4) & 7) * 2 + 16];
  case kSustain: return kRates[r[6] & 0x1f];
  default: return kRates[r[7] & 0x1f];
  }
}

// Runs the envelopes for |n| of the DSP's samples, and sets the mixer's
// volumes from them.
static void RunEnvelopes(unsigned n) {
  for (int v = 0; v < 8; v++) {
    AudioVoice *voice = &g_audio_voices[v];
    Envelope *e = &envelopes[v];
    if (!voice->on)
      continue;
    if (e->state == kRelease) {
      e->level = e->level > 8 * n ? e->level - 8 * n : 0;
      if (!e->level)
        voice->on = false;
    } else if (e->state == kGain && !(dsp[v * 16 + 7] & 0x80)) {
      e->level = (dsp[v * 16 + 7] & 0x7f) * 16;
    } else {
      unsigned rate = EnvelopeRate(e, v);
      if (rate) {
        // (No division: the GBA does it in software, slowly.)
        unsigned t = e->counter + n;
        for (; t >= rate; t -= rate)
          EnvelopeStep(e, v);
        e->counter = t;
      }
    }
    // Sample, envelope (/2048) and volume (/128): at the scale of a
    // sample, /4 (see AudioVoice).
    int env = e->level;
    int l = (int8_t)dsp[v * 16 + 0] * env >> 13, r = (int8_t)dsp[v * 16 + 1] * env >> 13;
    voice->vol = (int32_t)((uint32_t)r << 16) + l;
  }
}

void Audio_Init(void) {
  SpcPlayer_Initialize(&player);

  REG_SOUNDCNT_X = 0x80;  // on (first: the rest ignore writes while off)
  // DMA sound A on the left and B on the right, at full volume, by timer 0,
  // their FIFOs emptied.
  REG_SOUNDCNT_H = 1 << 2 | 1 << 3 | 1 << 9 | 1 << 11 | 1 << 12 | 1 << 15;
  REG_DMADAD(1) = (uintptr_t)&REG_FIFO_A;
  REG_DMADAD(2) = (uintptr_t)&REG_FIFO_A + 4;
  REG_TMCNT_L(0) = 0x10000 - kAudioTimerPeriod;
  REG_TMCNT_H(0) = 0x80;
}

#if SM_AUDIO_LOG
// For tools/spcref.c: what the game asks of the sound driver, frame by frame.
static uint32_t log_frame;
#endif

void RtlApuWrite(uint32 adr, uint8 val) {
#if SM_AUDIO_LOG
  if (player.input_ports[adr & 3] != val)
    printf("audio: %u port %u %u\n", (unsigned)log_frame, (unsigned)(adr & 3), val);
#endif
  player.input_ports[adr & 3] = val;
}

void RtlApuUpload(const uint8 *p) {
#if SM_AUDIO_LOG
  printf("audio: %u upload %u\n", (unsigned)log_frame, (unsigned)(p - snes_rom));
#endif
  SpcPlayer_Upload(&player, p);
}

// A frame's samples, with the driver's ticks in between. Its timer ticks
// at 500 Hz; it runs two ticks at a time, at 250 Hz, for the time: its
// tempo, effects and envelopes count ticks, and its slides and vibrato
// interpolate between them, so they come out the same, if coarser.
static void MixFrame(int8_t *left, int8_t *right) {
  // 16.16, in the mixer's samples.
  enum { kTicksPerRun = 2, kSamplesPerTick = 65536u * kAudioRate / (500 / kTicksPerRun) };
  static uint32_t until_tick;
  int done = 0;
  while (done < kAudioFrameSamples) {
    if (until_tick < 0x10000) {
      SpcPlayer_Tick(&player, kTicksPerRun);
      until_tick += kSamplesPerTick;
    }
    int n = until_tick >> 16;
    if (n > kAudioFrameSamples - done)
      n = kAudioFrameSamples - done;
    if (n > kAudioChunk)
      n = kAudioChunk;
    envelope_time += n * kAudioDspSamples;
    RunEnvelopes(envelope_time >> 8);
    envelope_time &= 0xff;
    int mute = dsp[0x6c] & 0x40;
    Audio_MixChunk(left + done, right + done, n, mute ? 0 : (int8_t)dsp[0x0c],
                   mute ? 0 : (int8_t)dsp[0x1c]);
    done += n;
    until_tick -= n << 16;
  }
}

void Audio_Frame(void) {
#if SM_AUDIO_LOG
  log_frame++;
#endif
  // Up to three frames ahead of what's playing, which the VBlank interrupt
  // moves on from a frame at a time. A frame's worth a frame of the game,
  // or two when few are left: always two, after a frame of the game that
  // took two of the GBA's, would make the next take two as well.
  static uint8_t next;
  for (int n = g_audio_queued < 2 ? 2 : 1; n && g_audio_queued < 3; n--) {
    MixFrame(g_audio_buffers[0][next], g_audio_buffers[1][next]);
    next = (next + 1) & (kAudioBuffers - 1);
    REG_IME = 0;
    g_audio_queued++;
    REG_IME = 1;
  }
}
