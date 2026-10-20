// The game's sound, on the GBA: its sound driver (src/spc_player.c, the
// SNES's driver translated to C), with a GBA mixer in place of the SNES's
// DSP, playing the samples make_cartridge.py decoded. See audio.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  // 10512 Hz: a timer period of 1596 cycles, and 176 samples a GBA frame.
  kAudioTimerPeriod = 1596,
  kAudioRate = 16777216 / kAudioTimerPeriod,
  kAudioFrameSamples = 280896 / kAudioTimerPeriod,
  // The DSP's 32000 Hz samples a mixer sample, in 8.8 fixed point.
  kAudioDspSamples = 32000 * 256 / kAudioRate,
  kAudioBuffers = 4,
  // The mixer mixes at most this many samples at a time: about as many as
  // between the driver's ticks (audio.c).
  kAudioChunk = 48,
};

// A DSP voice, as the mixer plays it.
typedef struct AudioVoice {
  const int8_t *pcm;
  // In samples, 16.16: where it's at, how far it goes a sample, where it
  // ends, and how far back it loops from there (0: it doesn't).
  uint32_t pos, step, end, loop_len;
  // Its volume times its envelope, /4 so that eight voices' samples (-128
  // to 127) times them fit in 16 bits: right << 16 + left.
  int32_t vol;
  bool on, noise;
} AudioVoice;

void Audio_Init(void);
// Once a frame of the game: runs the driver and mixes ahead.
void Audio_Frame(void);

// audio_mix.iwram.c:
extern AudioVoice g_audio_voices[8];
extern int8_t g_audio_buffers[2][kAudioBuffers][kAudioFrameSamples];
extern volatile uint8_t g_audio_play, g_audio_queued;
// The DSP's samples between steps of its noise (by FLG), or 0 to hold.
extern uint16_t g_audio_noise_period;
// Mixes |n| (up to kAudioChunk) samples of the voices into |left| and
// |right|, with the master volume.
void Audio_MixChunk(int8_t *left, int8_t *right, int n, int master_l, int master_r);
// In the VBlank interrupt: plays the next buffer this frame.
void Audio_VBlank(void);
