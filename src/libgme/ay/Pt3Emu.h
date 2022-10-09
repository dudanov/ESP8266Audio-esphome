#pragma once

// Sinclair Spectrum PT3 music file emulator

#include "AyApu.h"
#include "common.h"
#include "../ClassicEmu.h"
#include <stack>

namespace gme {
namespace emu {
namespace ay {
namespace pt3 {

/* PT3 MODULE DATA DESCRIPTION */

template<typename T> struct LoopData {
  typedef T data_type;
  uint8_t loop;
  uint8_t end;
  T data[0];
};

struct SampleData {
  SampleData() = delete;
  SampleData(const SampleData &) = delete;
  bool VolumeSlide() const { return mData[0] & 0x80; }
  bool VolumeSlideUp() const { return mData[0] & 0x40; }
  uint8_t Noise() const { return (mData[0] >> 1) & 0x1F; }
  int8_t EnvelopeSlide() const {
    const int8_t tmp = mData[0] >> 1;
    return (tmp & 16) ? (tmp | ~15) : (tmp & 15);
  }
  bool EnvelopeMask() const { return mData[0] & 0x01; }
  bool NoiseMask() const { return mData[1] & 0x80; }
  bool ToneStore() const { return mData[1] & 0x40; }
  bool NoiseEnvelopeStore() const { return mData[1] & 0x20; }
  bool ToneMask() const { return mData[1] & 0x10; }
  int8_t Volume() const { return mData[1] & 0x0F; }
  int16_t Transposition() const { return get_le16(mTransposition); }

 private:
  uint8_t mData[2];
  uint8_t mTransposition[2];
};

using OrnamentData = int8_t;
using Sample = LoopData<SampleData>;
using Ornament = LoopData<OrnamentData>;
using SamplePlayer = LoopDataPlayer<Sample>;
using OrnamentPlayer = LoopDataPlayer<Ornament>;
using Position = uint8_t;

class PT3Module {
  class LengthCounter {
   public:
    // Return song length in frames.
    unsigned CountSongLength(const PT3Module *module, unsigned &loop);

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
  PT3Module() = delete;
  PT3Module(const PT3Module &) = delete;

  static const PT3Module *GetModule(const uint8_t *data, size_t size);
  static const PT3Module *FindTSModule(const uint8_t *data, size_t size);

  // Get module format subversion.
  uint8_t GetSubVersion() const;

  // Song name.
  void GetName(char *out) const { GmeFile::copyField(out, mName, sizeof(mName)); }

  // Song author.
  void GetAuthor(char *out) const { GmeFile::copyField(out, mAuthor, sizeof(mAuthor)); }

  // Get song global delay.
  uint8_t GetDelay() const { return mDelay; }

  bool HasNoteTable(uint8_t table) const { return mNoteTable == table; }

  // Begin position iterator.
  const Position *GetPositionBegin() const { return mPositions; }

  // Loop position iterator.
  const Position *GetPositionLoop() const { return mPositions + mLoop; }

  // End position iterator.
  const Position *GetPositionEnd() const { return mPositions + mEnd; }

  // Get pattern index by specified number.
  const Pattern *GetPattern(const Position *it) const {
    return reinterpret_cast<const Pattern *>(mPattern.GetPointer(this) + *it);
  }

  // Get data from specified pattern.
  const PatternData *GetPatternData(const Pattern *pattern, uint8_t channel) const {
    return pattern->GetData(this, channel);
  }

  // Get sample by specified number.
  const Sample *GetSample(uint8_t number) const { return mSamples[number].GetPointer(this); }

  // Get data of specified ornament number.
  const Ornament *GetOrnament(uint8_t number) const { return mOrnaments[number].GetPointer(this); }

  // Return song length in frames.
  unsigned CountSongLength(unsigned &loop) const { return LengthCounter().CountSongLength(this, loop); }

  // Return song length in miliseconds.
  unsigned CountSongLengthMs(unsigned &loop) const;

 private:
  /* PT3 MODULE HEADER DATA */

  // Identification: "ProTracker 3.".
  uint8_t mIdentify[13];
  // Subversion: "3", "4", "5", "6", etc.
  uint8_t mSubVersion;
  // " compilation of " or any text of this length.
  uint8_t mUnused0[16];
  // Track name. Unused characters are padded with spaces.
  char mName[32];
  // " by " or any text of this length.
  uint8_t mUnused1[4];
  // Author's name. Unused characters are padded with spaces.
  char mAuthor[32];
  // One space (any character).
  uint8_t mUnused2;
  // Note frequency table number.
  uint8_t mNoteTable;
  // Delay value (tempo).
  uint8_t mDelay;
  // Song end position. Not used in player.
  uint8_t mEnd;
  // Song loop position.
  uint8_t mLoop;
  // Pattern table offset.
  DataOffset<DataOffset<PatternData>> mPattern;
  // Sample offsets. Starting from sample #0.
  DataOffset<Sample> mSamples[32];
  // Ornament offsets. Starting from ornament #0.
  DataOffset<Ornament> mOrnaments[16];
  // List of positions. Contains the pattern numbers (0...84) multiplied by 3. The table ends with 0xFF.
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

  void SetupVibrato() {
    mVibratoCounter = mVibratoOnTime = PatternCode();
    mVibratoOffTime = PatternCode();
    mToneSlide.Disable();
  }

  void RunVibrato() {
    if (mVibratoCounter && !--mVibratoCounter)
      mVibratoCounter = (mEnable = !mEnable) ? mVibratoOnTime : mVibratoOffTime;
  }

  uint16_t PlayTone(const Player *player);
  int16_t GetToneSlide() const { return mToneSlide.GetValue(); }
  void SetupGliss(const Player *player);
  void SetupPortamento(const Player *player, uint8_t prevNote, int16_t prevSliding);

 private:
  void mDisableVibrato() { mVibratoCounter = 0; }
  void mRunPortamento();
  const uint8_t *mPatternIt;
  SamplePlayer mSamplePlayer;
  OrnamentPlayer mOrnamentPlayer;
  DelayRunner mSkip;
  DelayedSlider<int16_t> mToneSlide;
  int16_t mTranspositionAccumulator, mToneDelta;
  uint8_t mVibratoCounter, mVibratoOnTime, mVibratoOffTime;
  uint8_t mVolume, mNote, mNoteSlide, mNoiseSlideStore;
  int8_t mAmplitudeSlideStore, mEnvelopeSlideStore;
  bool mEnable, mEnvelopeEnable, mPortamento;
};

class Player {
 public:
  void Load(const PT3Module *module) { mModule = module; }
  void Init() { mInit(); }
  void SetVolume(double volume) { mApu.SetVolume(volume); }
  void SetOscOutput(int idx, BlipBuffer *out) { mApu.SetOscOutput(idx, out); }
  void EndFrame(blip_clk_time_t time) { mApu.EndFrame(time); }
  void RunUntil(blip_clk_time_t time) {
    if (mDelay.Tick())
      mPlayPattern(time);
    mPlaySamples(time);
  }

  int16_t GetNotePeriod(uint8_t tone) const;
  uint8_t GetSubVersion() const { return mModule->GetSubVersion(); }
  void GetName(char *out) const { return mModule->GetName(out); }
  void GetAuthor(char *out) const { return mModule->GetAuthor(out); }
  unsigned CountSongLength(unsigned &loop) const { return mModule->CountSongLength(loop); }
  unsigned CountSongLengthMs(unsigned &loop) const { return mModule->CountSongLengthMs(loop); }

 private:
  void mInit();
  void mSetupEnvelope(Channel &channel);
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
  const PT3Module *mModule;
  // Song position iterators
  const Position *mPositionIt;
  DelayedSlider<int16_t> mEnvelopeSlider;
  DelayRunner mDelay;
  uint16_t mEnvelopeBase;
  uint8_t mNoiseBase;
};

class Pt3Emu : public ClassicEmu {
 public:
  Pt3Emu();
  ~Pt3Emu();
  static MusicEmu *createPt3Emu() { return BLARGG_NEW Pt3Emu; }
  static gme_type_t static_type() { return gme_pt3_type; }

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

}  // namespace pt3
}  // namespace ay
}  // namespace emu
}  // namespace gme
