// YM2413 FM sound chip emulator interface

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#pragma once

namespace gme {
namespace emu {
namespace vgm {

class Ym2413Emu {
  struct OPLL *opll;

 public:
  Ym2413Emu();
  ~Ym2413Emu();

  // Set output sample rate and chip clock rates, in Hz. Returns non-zero
  // if error.
  const char *set_rate(double sample_rate, double clock_rate);

  // Reset to power-up state
  void reset();

  // Mute voice n if bit n (1 << n) of mask is set
  enum { channel_count = 14 };
  void mute_voices(int mask);

  // Write 'data' to 'addr'
  void write(int addr, int data);

  // Run and write pair_count samples to output
  typedef short sample_t;
  enum { OUT_CHANNELS_NUM = 2 };  // stereo
  void run(int pair_count, sample_t *out);
};

}  // namespace vgm
}  // namespace emu
}  // namespace gme
