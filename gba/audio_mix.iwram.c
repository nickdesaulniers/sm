// The audio mixer, ARM code in IWRAM, as it runs for every sample. (So no
// division: see cmake/compiler-rt.cmake.)
#include "audio.h"

#include "gba_hw.h"

AudioVoice g_audio_voices[8] IWRAM_BSS;
int8_t g_audio_buffers[2][kAudioBuffers][kAudioFrameSamples];
volatile uint8_t g_audio_play IWRAM_BSS, g_audio_queued IWRAM_BSS;

static int8_t silence[kAudioFrameSamples];
uint16_t g_audio_noise_period IWRAM_BSS;
// The DSP's noise, as src/snes/dsp.c's dsp_handleNoise: a 15-bit LFSR, and
// the DSP's samples (8.8) since it stepped.
static int16_t noise IWRAM_DATA = -0x4000;
static uint32_t noise_time IWRAM_BSS;

static void Noise(int8_t *out, int n) {
  uint32_t period = g_audio_noise_period << 8;
  for (int i = 0; i < n; i++) {
    noise_time += kAudioDspSamples;
    for (; period && noise_time >= period; noise_time -= period) {
      int bit = (noise ^ (noise >> 1)) & 1;
      noise = (int16_t)((((noise >> 1) & 0x3fff) | bit << 14) << 1) >> 1;
    }
    out[i] = noise >> 7;
  }
}

void Audio_MixChunk(int8_t *left, int8_t *right, int n, int master_l, int master_r) {
  // Both sides in a word, left in the low half: a multiply a sample.
  int32_t sum[kAudioChunk];
  int8_t noise_samples[kAudioChunk];
  bool noise_done = false;
  for (int i = 0; i < n; i++)
    sum[i] = 0;
  for (int v = 0; v < 8; v++) {
    AudioVoice *voice = &g_audio_voices[v];
    if (!voice->on)
      continue;
    int32_t vol = voice->vol;
    if (voice->noise) {
      if (!noise_done)
        Noise(noise_samples, n);
      noise_done = true;
      for (int i = 0; i < n; i++)
        sum[i] += noise_samples[i] * vol;
      continue;
    }
    const int8_t *pcm = voice->pcm;
    uint32_t pos = voice->pos, step = voice->step, end = voice->end;
    for (int i = 0; i < n; i++) {
      sum[i] += pcm[pos >> 16] * vol;
      pos += step;
      if (pos >= end) {
        if (!voice->loop_len) {
          voice->on = false;
          break;
        }
        pos -= voice->loop_len;
      }
    }
    voice->pos = pos;
  }
  // The voices' sum, *4, is at the scale of the SNES's 16-bit samples; the
  // master volume is /128. Four times as loud as that: the GBA's 8 bits are
  // few, and there's no echo to add to it.
  for (int i = 0; i < n; i++) {
    int l = (int16_t)sum[i], r = (sum[i] - l) >> 16;
    l = (l * master_l) >> 11;
    r = (r * master_r) >> 11;
    left[i] = l < -128 ? -128 : l > 127 ? 127 : l;
    right[i] = r < -128 ? -128 : r > 127 ? 127 : r;
  }
}

void Audio_VBlank(void) {
  const int8_t *l = silence, *r = silence;
  if (g_audio_queued) {
    l = g_audio_buffers[0][g_audio_play];
    r = g_audio_buffers[1][g_audio_play];
    g_audio_play = (g_audio_play + 1) & (kAudioBuffers - 1);
    g_audio_queued--;
  }
  // A frame's buffer lasts exactly the frame (see kAudioTimerPeriod).
  REG_DMACNT(1) = 0;
  REG_DMASAD(1) = (uintptr_t)l;
  REG_DMACNT(1) = kDmaDestFixed | kDmaRepeat | kDma32 | kDmaAtSoundFifo | kDmaEnable;
  REG_DMACNT(2) = 0;
  REG_DMASAD(2) = (uintptr_t)r;
  REG_DMACNT(2) = kDmaDestFixed | kDmaRepeat | kDma32 | kDmaAtSoundFifo | kDmaEnable;
}
