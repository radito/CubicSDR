// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <cstddef>

#include "DemodDefs.h"
#include "DenoiseMode.h"
#include "RtAudio.h"
#include "ThreadBlockingQueue.h"

extern "C" {
#include "rnnoise.h"
}

class AudioThreadInput {
 public:
  long long frequency{};
  int inputRate{};
  int sampleRate{};
  int channels{};
  float peak{};
  int type{};
  bool is_squelch_active{};
  bool is_denoised{};
  bool discontinuity{};

  std::vector<float> data;
  std::vector<float> dataOut;

  AudioThreadInput()
      : frequency(0), inputRate(0), sampleRate(0), channels(0), peak(0), type(0), is_squelch_active(false), is_denoised(false), discontinuity(false) {}

  explicit AudioThreadInput(AudioThreadInput* copyFrom) { copy(copyFrom); }

  void copy(AudioThreadInput* copyFrom) {
    frequency = copyFrom->frequency;
    inputRate = copyFrom->inputRate;
    sampleRate = copyFrom->sampleRate;
    channels = copyFrom->channels;
    peak = copyFrom->peak;
    type = copyFrom->type;
    is_squelch_active = copyFrom->is_squelch_active;
    is_denoised = copyFrom->is_denoised;
    discontinuity = copyFrom->discontinuity;
    data.assign(copyFrom->data.begin(), copyFrom->data.end());
  }

  virtual ~AudioThreadInput() = default;
};

typedef std::shared_ptr<AudioThreadInput> AudioThreadInputPtr;

typedef ThreadBlockingQueue<AudioThreadInputPtr> DemodulatorThreadOutputQueue;

typedef std::shared_ptr<DemodulatorThreadOutputQueue> DemodulatorThreadOutputQueuePtr;

class AudioThreadCommand {
 public:
  enum class Type { AUDIO_THREAD_CMD_NULL, AUDIO_THREAD_CMD_SET_DEVICE, AUDIO_THREAD_CMD_SET_SAMPLE_RATE };

  AudioThreadCommand() : cmdType(AudioThreadCommand::Type::AUDIO_THREAD_CMD_NULL), int_value(0) {}

  AudioThreadCommand::Type cmdType;
  int int_value;
};

typedef ThreadBlockingQueue<AudioThreadInputPtr> AudioThreadInputQueue;
typedef ThreadBlockingQueue<AudioThreadCommand> AudioThreadCommandQueue;

typedef std::shared_ptr<AudioThreadInputQueue> AudioThreadInputQueuePtr;
typedef std::shared_ptr<AudioThreadCommandQueue> AudioThreadCommandQueuePtr;

class AudioStereoRing {
 public:
  explicit AudioStereoRing(size_t capacityFrames = 32768);
  bool push(float left, float right);
  bool pop(float& left, float& right);
  // Consumer-side block reads: one acquire/release pair per call instead of
  // per frame. Both return the number of frames taken.
  size_t pop(float* interleaved, size_t frames);
  size_t discard(size_t frames);
  // Called by the consumer; also applies any producer-requested flush.
  size_t available();
  // Observes logical fill without modifying the consumer cursor.
  size_t producerAvailable() const;
  size_t capacity() const;
  void clear();

 private:
  std::vector<float> samples;
  const size_t capacityFrames;
  std::atomic<size_t> readPosition{0};
  std::atomic<size_t> writePosition{0};
  std::atomic<size_t> discardBefore{0};
};

class AudioMixState {
 public:
  AudioMixState();
  void queue(const AudioThreadInput& input);
  void clear();
  // Adds `frames` of playback into interleaved stereo `out`. Only the audio
  // callback may call this: it owns the ring's read side and playout state.
  void render(float* out, size_t frames);

  AudioStereoRing ring;
  std::atomic_bool active{false};
  std::atomic_bool primed{false};
  std::atomic<float> gain{1.0f};
  std::atomic_int sampleRate{0};
  std::atomic_size_t overflows{0};
  std::atomic_size_t underflows{0};
  // Underflows where audio resumed within a moment: audible gaps mid-stream,
  // unlike the ring simply running empty when a squelch closes.
  std::atomic_size_t dropouts{0};
  // Playout diagnostics, published by render() once per second.
  std::atomic<float> bufferedMs{0.0f};
  std::atomic<float> rateTrimPpm{0.0f};
  std::atomic_size_t catchUps{0};

 private:
  void clearUnlocked();
  void startPlayout();
  void updateRateTrim(size_t callbackFrames, int rate);

  // Consumer-only playout state. SDR and sound card clocks never match
  // exactly and stalls leave backlogs, so the read rate is trimmed slightly
  // to hold the ring's minimum fill near a small target.
  static constexpr size_t kRenderChunkFrames = 4096;
  static constexpr size_t kInterpolationTaps = 16;
  std::vector<float> scratch;
  // Last kInterpolationTaps frames, stored twice so any window of them is
  // contiguous; output sits between the 8th and 9th frame of the window.
  std::array<float, kInterpolationTaps * 4> history{};
  size_t historyHead = 0;
  double position = 0.0;  // fractional read position within the window
  double readRatio = 1.0;
  bool correcting = false;
  size_t jitterMarginFrames = 0;
  size_t windowMinimumFill = 0;
  size_t windowFillSum = 0;
  size_t windowCallbacks = 0;
  size_t windowFrames = 0;
  bool starving = false;
  size_t starvedFrames = 0;

  std::mutex producerMutex;
  bool havePrevious = false;
  float previousLeft = 0.0f;
  float previousRight = 0.0f;
  double phase = 0.0;
  int resamplerInputRate = 0;
  int resamplerOutputRate = 0;
};

class AudioThread : public IOThread {
 public:
  static constexpr size_t MAX_MIX_SOURCES = 64;
  AudioThread();
  ~AudioThread() override;

  static void enumerateDevices(std::vector<RtAudio::DeviceInfo>& devs);

  void setInitOutputDevice(int deviceId, int sampleRate_in = -1);
  int getOutputDevice();

  int getSampleRate();

  void run() override;
  void terminate() override;

  bool isActive();
  void setActive(bool state);

  void setGain(float gain_in);

  void setDenoiseMode(DenoiseMode mode);
  void setDenoiseEnabled(bool enabled) {
    setDenoiseMode(enabled ? DenoiseMode::Strong : DenoiseMode::Off);
  }

  static std::map<int, int> deviceSampleRate;

  AudioThreadCommandQueue* getCommandQueue();

  // give access to the this AudioThread lock
  std::recursive_mutex& getMutex();

  static void deviceCleanup();
  static void setDeviceSampleRate(int deviceId, int sampleRate);

  //
  void attachControllerThread(std::thread* controllerThread);

  // fields below, only to be used by other AudioThreads !
  std::atomic_size_t underflowCount;
  // protected by m_mutex
  std::vector<AudioThread*> boundThreads;
  AudioThreadInputQueuePtr inputQueue;

  AudioMixState* getMixSource(size_t index) const {
    return mixSources[index].load(std::memory_order_acquire);
  }
  AudioMixState* protectMixSource(size_t index) {
    for (;;) {
      AudioMixState* state = mixSources[index].load(std::memory_order_seq_cst);
      mixHazards[index].store(state, std::memory_order_seq_cst);
      if (mixSources[index].load(std::memory_order_seq_cst) == state) return state;
    }
  }
  void releaseMixSource(size_t index) {
    mixHazards[index].store(nullptr, std::memory_order_seq_cst);
  }

  int debug;

 private:
  static constexpr size_t RNNOISE_FRAME_SIZE = 480;

  std::atomic_bool active;
  std::atomic_int outputDevice;
  std::atomic<DenoiseMode> denoiseMode{DenoiseMode::Off};

  RtAudio dac;
  unsigned int nBufferFrames;
  RtAudio::StreamOptions opts;
  RtAudio::StreamParameters parameters;
  AudioThreadCommandQueue cmdQueue;
  int sampleRate;

  // if != nullptr, it mean AudioThread is a controller thread.
  std::thread* controllerThread;

  // The own m_mutex protecting this AudioThread, in particular boundThreads
  std::recursive_mutex m_mutex;
  std::shared_ptr<AudioMixState> mixState;
  std::array<std::atomic<AudioMixState*>, MAX_MIX_SOURCES> mixSources{};
  std::array<std::atomic<AudioMixState*>, MAX_MIX_SOURCES> mixHazards{};
  std::vector<std::shared_ptr<AudioMixState>> mixStateOwners;

  // RNNoise runs on the per-demodulator AudioThread, outside CoreAudio's
  // real-time callback.
  std::mutex denoiseMutex;
  void* denoiseState = nullptr;
  DenoiseMode denoiseProcessorMode = DenoiseMode::Off;
  std::array<float, RNNOISE_FRAME_SIZE> denoiseFrame{};
  size_t denoiseFrameFill = 0;
  int denoiseProcessorSampleRate = 0;
  msresamp_rrrf denoiseInputResampler = nullptr;
  msresamp_rrrf denoiseOutputResampler = nullptr;
  std::vector<float> denoiseModelInput;
  std::vector<float> denoiseModelOutput;
  ReBuffer<AudioThreadInput> denoiseOutputBuffers{"RNNoiseOutputBuffers"};

  struct PlayoutCounters {
    size_t dropouts = 0;
    size_t overflows = 0;
    size_t catchUps = 0;
  };
  PlayoutCounters reportedPlayout;
  std::chrono::steady_clock::time_point lastPlayoutCheck{};
  std::chrono::steady_clock::time_point lastPlayoutReport{};

  void setupDevice(int deviceId);
  void setSampleRate(int sampleRate_in);
  void processAudioInput(const AudioThreadInputPtr& input);
  void reportPlayout();
  void queuePlaybackInput(const AudioThreadInputPtr& input);
  void resetDenoiseProcessorLocked();

  void bindThread(AudioThread* other);
  void removeThread(AudioThread* other);

  static std::map<int, AudioThread*> deviceController;

  // The mutex protecting static deviceController, deviceThread and deviceSampleRate access.
  static std::recursive_mutex m_device_mutex;
};
