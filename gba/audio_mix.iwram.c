// The audio mixer, ARM code in IWRAM, as it runs for every sample. (So no
// division: see cmake/compiler-rt.cmake.)
#include "audio.h"

#include "gba_hw.h"

#include <stddef.h>
#include <string.h>

AudioVoice g_audio_voices[8] IWRAM_BSS;
// (Words, for the DMA, as silence's.)
int8_t g_audio_buffers[2][kAudioBuffers][kAudioFrameSamples] __attribute__((aligned(4)));
volatile uint8_t g_audio_play IWRAM_BSS, g_audio_queued IWRAM_BSS;

AudioMix g_audio_mix IWRAM_BSS;

// (In ROM: the DMA reads it as well from there, and EWRAM is full.)
static const int8_t silence[kAudioFrameSamples] __attribute__((aligned(4)));
// The echo: the voices' echo, delayed (in EWRAM: up to 5 KB), both sides
// in a word, in the units of the voices' sums (/2 of the DSP's).
static uint32_t echo_buf[kAudioEchoMax];
static uint16_t echo_pos IWRAM_BSS;
typedef struct EchoState {
  int32_t fir0, fir1, feedback;
  uint32_t old, last;
} EchoState;
static EchoState echo_state IWRAM_BSS;
void Audio_EchoPairs(int32_t *wet, int pairs, uint32_t *buf, EchoState *s);
void Audio_ClearSums(int32_t *sums, int n);
void Audio_MixVoice(int32_t *sum, int n, AudioVoice *v);
void Audio_OutputEcho(int8_t *left, int8_t *right, const int32_t *sums, int n, const int32_t vol[4]);
// As audio_mix.S has them.
_Static_assert(offsetof(AudioVoice, vol) == 20 && offsetof(AudioVoice, on) == 24, "AudioVoice");
_Static_assert(offsetof(EchoState, last) == 16, "EchoState");

ROM_CODE void Audio_ClearEcho(void) {
  memset(echo_buf, 0, sizeof(echo_buf));
  echo_state.old = echo_state.last = 0;
  echo_pos = 0;
}


// As src/snes/dsp.c's dsp_handleEcho, on pairs of samples (see echo.S):
// what comes out of the echo buffer, filtered, goes back in with the voices'
// echo (|wet|) at the feedback volume, and to wet[kAudioChunk] on, to play
// at the echo volume.
static void Echo(int32_t *wet, int n) {
  const AudioMix *m = &g_audio_mix;
  if (!m->echo_writes) {
    // Only while the driver changes the echo's length, its volume 0.
    Audio_ClearSums(wet + kAudioChunk, n);
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

// Adds |n| of the noise's samples, times |vol|, to |sum|; |samples| has them
// once they're made (|first|: not yet), for the voices after the first with
// noise. (Thumb code, from the cartridge: only a few of the sounds have
// noise.)
ROM_CODE __attribute__((noinline)) static void AddNoise(int32_t *sum, int n, int32_t vol,
                                                        int8_t *samples, bool first) {
  if (first) {
    uint32_t period = g_audio_noise_period << 8;
    for (int i = 0; i < n; i++) {
      noise_time += kAudioDspSamples;
      for (; period && noise_time >= period; noise_time -= period) {
        int bit = (noise ^ (noise >> 1)) & 1;
        noise = (int16_t)((((noise >> 1) & 0x3fff) | bit << 14) << 1) >> 1;
      }
      samples[i] = noise >> 7;
    }
  }
  for (int i = 0; i < n; i++)
    sum[i] += samples[i] * vol;
}

void Audio_MixChunk(int8_t *left, int8_t *right, int n) {
  __builtin_assume(n >= 2);  // (so there is one loop for the voices, not two)
  const AudioMix *m = &g_audio_mix;
  bool echo = m->echo_on;
  // Both sides in a word, left in the low half: a multiply a sample. With
  // the echo on, the voices with echo add to |wet|, the others to |dry|;
  // the echo's output follows (see Echo).
  int32_t sums[3 * kAudioChunk], *dry = sums, *wet = sums + kAudioChunk;
  int8_t noise_samples[kAudioChunk];
  bool noise_done = false;
  Audio_ClearSums(dry, n);
  Audio_ClearSums(wet, n);
#pragma clang loop unroll(disable)
  for (int v = 0; v < 8; v++) {
    AudioVoice *voice = &g_audio_voices[v];
    if (!voice->on)
      continue;
    int32_t vol = voice->vol;
    int32_t *sum = dry + (echo & voice->echo) * kAudioChunk;  // (|wet| follows |dry|)
    if (voice->noise) {
      AddNoise(sum, n, vol, noise_samples, !noise_done);
      noise_done = true;
      continue;
    }
    Audio_MixVoice(sum, n, voice);
  }
  // The voices' sums are at half the scale of the SNES's (see AudioVoice),
  // and the master and echo volumes are /128: /8192 makes the SNES's 16
  // bits /128, twice as loud as its top 8 bits, as the GBA's 8 are few.
  // (Without the echo, it has no volume, and |wet| nothing in it.)
  int32_t vol[4] = {m->master_l, m->master_r, echo ? m->echo_l : 0, echo ? m->echo_r : 0};
  if (echo)
    Echo(wet, n);
  Audio_OutputEcho(left, right, sums, n, vol);
}

uint32_t g_audio_underruns IWRAM_BSS;

ROM_CODE void Audio_VBlank(void) {
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
