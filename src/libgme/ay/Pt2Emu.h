#pragma once

// Sinclair Spectrum PT2 music file emulator

#include "AyApu.h"
#include "common.h"
#include "../ClassicEmu.h"
#include <stack>

namespace gme {
namespace emu {
namespace ay {
namespace pt2 {

/* PT2 MODULE DATA DESCRIPTION */

template<typename T> struct LoopData {
  uint8_t end;
  uint8_t loop;
  T data[0];
};

template<typename T> class LoopDataPlayer {
 public:
  inline void Load(const LoopData<T> *data) {
    mData = data->data;
    mPos = 0;
    mEnd = data->end;
    mLoop = data->loop;
  }
  inline void SetPosition(uint8_t pos) { mPos = pos; }
  inline const T &GetData() const { return mData[mPos]; }
  inline void Advance() {
    if (++mPos >= mEnd)
      mPos = mLoop;
  }

 private:
  const T *mData;
  uint8_t mPos, mEnd, mLoop;
};

struct SampleData {
  SampleData() = delete;
  SampleData(const SampleData &) = delete;
  bool ToneMask() const { return mData[0] & 2; }
  bool NoiseMask() const { return mData[0] & 1; }
  uint8_t Noise() const { return mData[0] / 8; }
  uint8_t Volume() const { return mData[1] / 16; }
  int16_t Transposition() const {
    const int16_t tmp = mData[1] % 16 * 256 + mData[2];
    return (mData[0] & 4) ? tmp : -tmp;
  }

 private:
  uint8_t mData[3];
};

using PatternData = uint8_t;
using OrnamentData = int8_t;
using Sample = LoopData<SampleData>;
using Ornament = LoopData<OrnamentData>;
using SamplePlayer = LoopDataPlayer<SampleData>;
using OrnamentPlayer = LoopDataPlayer<OrnamentData>;
using Position = uint8_t;

class PT2Module {
  class LengthCounter {
   public:
    // Return song length in frames.
    unsigned CountSongLength(const PT2Module *module, unsigned &loop);

   private:
    unsigned mCountPositionLength();
    struct Channel {
      const PatternData *data;
      DelayRunner delay;
    };
    std::array<Channel, AyApu::OSCS_NUM> mChannels;
    std::stack<PatternData> mStack;
    uint8_t mDelay;
  };

 public:
  PT2Module() = delete;
  PT2Module(const PT2Module &) = delete;

  static const int16_t NOTE_TABLE[96];
  static int16_t GetNotePeriod(uint8_t tone);

  static const PT2Module *GetModule(const uint8_t *data, size_t size);
  static const PT2Module *FindTSModule(const uint8_t *data, size_t size);

  // Song name.
  void GetName(char *out) const { GmeFile::copyField(out, mName, sizeof(mName)); }

  // Get song global delay.
  uint8_t GetDelay() const { return mDelay; }

  // Begin position iterator.
  const Position *GetPositionBegin() const { return mPositions; }

  // Loop position iterator.
  const Position *GetPositionLoop() const { return mPositions + mLoop; }

  // End position iterator.
  const Position *GetPositionEnd() const { return mPositions + mEnd; }

  // Get pattern index by specified number.
  const Pattern *GetPattern(const Position *it) const { return mPattern.GetPointer<Pattern>(this) + *it; }

  // Get data from specified pattern.
  const PatternData *GetPatternData(const Pattern *pattern, uint8_t channel) const {
    return pattern->GetOffset(channel).GetPointer<PatternData>(this);
  }

  // Get sample by specified number.
  const Sample *GetSample(uint8_t number) const { return mSamples[number].GetPointer<Sample>(this); }

  // Get data of specified ornament number.
  const Ornament *GetOrnament(uint8_t number) const { return mOrnaments[number].GetPointer<Ornament>(this); }

  // Return song length in frames.
  unsigned CountSongLength(unsigned &loop) const { return LengthCounter().CountSongLength(this, loop); }

  // Return song length in miliseconds.
  unsigned CountSongLengthMs(unsigned &loop) const;

 private:
  /* PT2 MODULE HEADER DATA */

  // Delay value (tempo).
  uint8_t mDelay;
  // Song end position. Not used in player.
  uint8_t mEnd;
  // Song loop position.
  uint8_t mLoop;
  // Sample offsets. Starting from sample #0.
  DataOffset mSamples[32];
  // Ornament offsets. Starting from ornament #0.
  DataOffset mOrnaments[16];
  // Pattern table offset.
  DataOffset mPattern;
  // Track name. Unused characters are padded with spaces.
  char mName[30];
  // List of positions. Contains the pattern numbers. The table ends with 0xFF.
  Position mPositions[0];
};

class Player;

// Channel entity
struct Channel {
  /* only create */
  Channel() = default;
  /* not allow make copies */
  Channel(const Channel &) = delete;

  void Reset();
  void SetNote(uint8_t note) { mNote = note; }
  uint8_t GetNote() const { return mNote; }
  void Enable() { mEnable = true; }
  void Disable() { mEnable = false; }
  bool IsEnabled() const { return mEnable; }
  void SlideEnvelope(int8_t &value);
  uint8_t SlideNoise();
  uint8_t SlideAmplitude();

  void SetPatternData(const uint8_t *data) { mPatternIt = data; }
  void mSkipPatternCode(size_t n) { mPatternIt += n; }
  uint8_t PatternCode() { return *mPatternIt++; }

  int16_t PatternCodeLE16() {
    const int16_t value = get_le16(mPatternIt);
    mPatternIt += 2;
    return value;
  }

  int16_t PatternCodeBE16() {
    const int16_t value = get_be16(mPatternIt);
    mPatternIt += 2;
    return value;
  }

  bool IsEmptyLocation() { return !mSkip.Tick(); }
  void SetSkipLocations(uint8_t skip) { mSkip.Set(skip); }

  void SetSample(const Sample *sample) { mSamplePlayer.Load(sample); }
  void SetSamplePosition(uint8_t pos) { mSamplePlayer.SetPosition(pos); }
  void SetOrnament(const Ornament *ornament) { mOrnamentPlayer.Load(ornament); }
  void SetOrnamentPosition(uint8_t pos) { mOrnamentPlayer.SetPosition(pos); }
  const SampleData &GetSampleData() const { return mSamplePlayer.GetData(); }

  void Advance() {
    mSamplePlayer.Advance();
    mOrnamentPlayer.Advance();
  }

  bool IsEnvelopeEnabled() const { return mEnvelopeEnable; }
  void EnvelopeEnable() { mEnvelopeEnable = true; }
  void EnvelopeDisable() { mEnvelopeEnable = false; }

  uint8_t GetVolume() const { return mVolume; }
  void SetVolume(uint8_t volume) { mVolume = volume; }

  uint16_t PlayTone(const Player *player);
  int16_t GetToneSlide() const { return mToneSlide.GetValue(); }
  void SetupGliss(const Player *player);
  void SetupPortamento(const Player *player, uint8_t prevNote, int16_t prevSliding);

 private:
  void mRunPortamento();
  const uint8_t *mPatternIt;
  SamplePlayer mSamplePlayer;
  OrnamentPlayer mOrnamentPlayer;
  DelayRunner mSkip;
  SimpleSlider mToneSlide;
  int16_t mToneDelta;
  uint8_t mVolume, mNote, mNoteSlide, mNoiseSlideStore;
  int8_t mAmplitudeSlideStore, mEnvelopeSlideStore;
  bool mEnable, mEnvelopeEnable, mPortamento;
};

class Player {
 public:
  void Load(const PT2Module *module) { mModule = module; }
  void Init() { mInit(); }
  void SetVolume(double volume) { mApu.SetVolume(volume); }
  void SetOscOutput(int idx, BlipBuffer *out) { mApu.SetOscOutput(idx, out); }
  void EndFrame(blip_clk_time_t time) { mApu.EndFrame(time); }
  void RunUntil(blip_clk_time_t time) {
    if (mDelay.Tick())
      mPlayPattern(time);
    mPlaySamples(time);
  }

  void GetName(char *out) const { return mModule->GetName(out); }
  unsigned CountSongLength(unsigned &loop) const { return mModule->CountSongLength(loop); }
  unsigned CountSongLengthMs(unsigned &loop) const { return mModule->CountSongLengthMs(loop); }

 private:
  void mInit();
  void mPlayPattern(blip_clk_time_t time);
  void mPlaySamples(blip_clk_time_t time);
  void mAdvancePosition();
  uint8_t mGetAmplitude(uint8_t volume, uint8_t amplitude) const;
  // AY APU Emulator
  AyApu mApu;
  // Channels
  std::array<Channel, AyApu::OSCS_NUM> mChannels;
  // Pattern commands stack
  std::stack<uint8_t> mCmdStack;
  // Song file header
  const PT2Module *mModule;
  // Song position iterators
  const Position *mPositionIt;
  DelayRunner mDelay;
  uint16_t mEnvelopeBase;
  uint8_t mNoiseBase;
};

class Pt2Emu : public ClassicEmu {
 public:
  Pt2Emu();
  ~Pt2Emu();
  static MusicEmu *createPt2Emu() { return BLARGG_NEW Pt2Emu; }
  static gme_type_t static_type() { return gme_pt2_type; }

 protected:
  blargg_err_t mLoad(const uint8_t *data, long size) override;
  blargg_err_t mStartTrack(int) override;
  blargg_err_t mGetTrackInfo(track_info_t *, int track) const override;
  blargg_err_t mRunClocks(blip_clk_time_t &) override;
  void mSetTempo(double) override;
  void mSetChannel(int, BlipBuffer *, BlipBuffer *, BlipBuffer *) override;
  void mUpdateEq(BlipEq const &) override;

  /* PLAYER METHODS AND DATA */

 private:
  bool mCreateTS();
  void mDestroyTS();
  bool mHasTS() const { return mTurboSound != nullptr; }
  // Player
  Player mPlayer;
  // TurboSound player
  Player *mTurboSound;
  // Current emulation time
  blip_clk_time_t mEmuTime;
  // Play period 50Hz
  blip_clk_time_t mFramePeriod;
};

}  // namespace pt2
}  // namespace ay
}  // namespace emu
}  // namespace gme
