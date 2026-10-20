// The audio mixer, ARM code in IWRAM, as it runs for every sample. (So no
// division: see cmake/compiler-rt.cmake.)
#include "audio.h"

#include "gba_hw.h"

#include <stddef.h>
#include <string.h>

AudioVoice g_audio_voices[8] IWRAM_BSS;
int8_t g_audio_buffers[2][kAudioBuffers][kAudioFrameSamples];
volatile uint8_t g_audio_play IWRAM_BSS, g_audio_queued IWRAM_BSS;

AudioMix g_audio_mix IWRAM_BSS;

static int8_t silence[kAudioFrameSamples];
// The echo: the voices' echo, delayed (in EWRAM: up to 5 KB), both sides
// in a word, in the units of the voices' sums (/4 of the DSP's).
static uint32_t echo_buf[kAudioEchoMax];
static uint16_t echo_pos IWRAM_BSS;
typedef struct EchoState {
  int32_t fir0, fir1, feedback;
  uint32_t old, last;
} EchoState;
static EchoState echo_state IWRAM_BSS;
void Audio_EchoPairs(int32_t *wet, int pairs, uint32_t *buf, EchoState *s);
void Audio_MixVoice(int32_t *sum, int n, AudioVoice *v);
// As audio_mix.S has them.
_Static_assert(offsetof(AudioVoice, vol) == 20 && offsetof(AudioVoice, on) == 24, "AudioVoice");
_Static_assert(offsetof(EchoState, last) == 16, "EchoState");

ROM_CODE void Audio_ClearEcho(void) {
  memset(echo_buf, 0, sizeof(echo_buf));
  echo_state.old = echo_state.last = 0;
  echo_pos = 0;
}

static inline int Clamp16(int x) {
  return x < -0x8000 ? -0x8000 : x > 0x7fff ? 0x7fff : x;
}

// As src/snes/dsp.c's dsp_handleEcho, on pairs of samples (see echo.S):
// what comes out of the echo buffer, filtered, goes back in with the voices'
// echo (|wet|) at the feedback volume, and to wet[kAudioChunk] on, to play
// at the echo volume.
static void Echo(int32_t *wet, int n) {
  const AudioMix *m = &g_audio_mix;
  if (!m->echo_writes) {
    // Only while the driver changes the echo's length, its volume 0.
    memset(wet + kAudioChunk, 0, n * 4);
    return;
  }
  echo_state.fir0 = m->fir[0];
  echo_state.fir1 = m->fir[1];
  echo_state.feedback = m->feedback;
  int pos = echo_pos, len = m->echo_len;
  for (int pairs = n >> 1; pairs;) {
    // Up to where the buffer wraps.
    int run = len - pos < pairs ? len - pos : pairs;
    Audio_EchoPairs(wet, run, echo_buf + pos, &echo_state);
    wet += 2 * run;
    pairs -= run;
    pos += run;
    if (pos >= len)
      pos = 0;
  }
  echo_pos = pos;
}

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

void Audio_MixChunk(int8_t *left, int8_t *right, int n) {
  const AudioMix *m = &g_audio_mix;
  bool echo = m->echo_l || m->echo_r || m->feedback;
  for (int v = 0; v < 8; v++)
    echo |= g_audio_voices[v].on & g_audio_voices[v].echo;
  // Both sides in a word, left in the low half: a multiply a sample. With
  // the echo on, the voices with echo add to |wet|, the others to |dry|.
  // (|wet| is followed by the echo's output: see Echo.)
  int32_t dry[kAudioChunk], wet[2 * kAudioChunk], *echo_out = wet + kAudioChunk;
  int8_t noise_samples[kAudioChunk];
  bool noise_done = false;
  for (int i = 0; i < n; i++)
    dry[i] = 0;
  for (int i = 0; echo && i < n; i++)
    wet[i] = 0;
  for (int v = 0; v < 8; v++) {
    AudioVoice *voice = &g_audio_voices[v];
    if (!voice->on)
      continue;
    int32_t vol = voice->vol;
    int32_t *sum = echo && voice->echo ? wet : dry;
    if (voice->noise) {
      if (!noise_done)
        Noise(noise_samples, n);
      noise_done = true;
      for (int i = 0; i < n; i++)
        sum[i] += noise_samples[i] * vol;
      continue;
    }
    Audio_MixVoice(sum, n, voice);
  }
  // The voices' sum, *4, is at the scale of the SNES's 16-bit samples; the
  // master and echo volumes are /128. Four times as loud as that: the GBA's
  // 8 bits are few.
  int master_l = m->master_l, master_r = m->master_r;
  if (!echo) {
    for (int i = 0; i < n; i++) {
      int32_t s = dry[i];
      int l = (int16_t)s, r = (s - l) >> 16;
      l = (l * master_l) >> 11;
      r = (r * master_r) >> 11;
      left[i] = l < -128 ? -128 : l > 127 ? 127 : l;
      right[i] = r < -128 ? -128 : r > 127 ? 127 : r;
    }
    return;
  }
  Echo(wet, n);
  int echo_l = m->echo_l, echo_r = m->echo_r;
  for (int i = 0; i < n; i++) {
    int32_t d = dry[i] + wet[i], e = echo_out[i];
    int dl = (int16_t)d, dr = (d - dl) >> 16;
    int el = (int16_t)e, er = (e - el) >> 16;
    int l = (dl * master_l + el * echo_l) >> 11;
    int r = (dr * master_r + er * echo_r) >> 11;
    left[i] = l < -128 ? -128 : l > 127 ? 127 : l;
    right[i] = r < -128 ? -128 : r > 127 ? 127 : r;
  }
}

uint32_t g_audio_underruns IWRAM_BSS;

void Audio_VBlank(void) {
  const int8_t *l = silence, *r = silence;
  if (!g_audio_queued)
    g_audio_underruns++;
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
