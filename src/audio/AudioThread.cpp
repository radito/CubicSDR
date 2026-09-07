// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "AudioThread.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include "CubicSDR.h"
#include "DemodulatorInstance.h"

//50 ms
#define HEARTBEAT_CHECK_PERIOD_MICROS (50 * 1000)

std::map<int, AudioThread*> AudioThread::deviceController;

std::map<int, int> AudioThread::deviceSampleRate;

std::recursive_mutex AudioThread::m_device_mutex;

AudioThread::AudioThread() : IOThread(), nBufferFrames(RNNOISE_FRAME_SIZE), sampleRate(0), controllerThread(nullptr), mixState(std::make_shared<AudioMixState>()) {

  underflowCount = 0;
  active.store(false);
  outputDevice.store(-1);

  for (auto& source : mixSources) source.store(nullptr, std::memory_order_relaxed);
  for (auto& hazard : mixHazards) hazard.store(nullptr, std::memory_order_relaxed);

  debug = 255;

  cout << "Initialize AudioThread" << std::endl;
}

AudioThread::~AudioThread() {

  mixState->active.store(false, std::memory_order_release);
  mixState->clear();

  {
    std::lock_guard<std::mutex> lock(denoiseMutex);
    resetDenoiseProcessorLocked();
  }

  if (controllerThread != nullptr) {

    //
    //NOT PROTECTED by m_mutex on purpose, to prevent deadlocks with controllerThread
    // it doesn't matter, it is only called when all "normal" audio threads are detached from the controller.
    //

    terminate();
    controllerThread->join();
    delete controllerThread;
    controllerThread = nullptr;
  }
}

std::recursive_mutex& AudioThread::getMutex() {
  return m_mutex;
}

void AudioThread::attachControllerThread(std::thread* controllerThread_in) {

  controllerThread = controllerThread_in;
}

void AudioThread::bindThread(AudioThread* other) {

  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  const auto owner = other->mixState;
  bool sourceBound = false;
  for (auto& source : mixSources) {
    AudioMixState* expected = nullptr;
    if (source.compare_exchange_strong(expected, owner.get(), std::memory_order_seq_cst) ||
        expected == owner.get()) {
      sourceBound = true;
      break;
    }
  }
  if (!sourceBound) return;

  if (std::find(boundThreads.begin(), boundThreads.end(), other) == boundThreads.end()) {
    boundThreads.push_back(other);
  }
  if (std::find(mixStateOwners.begin(), mixStateOwners.end(), owner) == mixStateOwners.end()) {
    mixStateOwners.push_back(owner);
  }
}

void AudioThread::removeThread(AudioThread* other) {

  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  AudioMixState* removedState = other->mixState.get();
  for (auto& source : mixSources) {
    if (source.load(std::memory_order_seq_cst) == removedState) {
      source.store(nullptr, std::memory_order_seq_cst);
    }
  }

  // A callback that observed the old source either publishes its hazard
  // before the removal above or rechecks the slot and abandons the pointer.
  // Waiting is confined to the control thread; CoreAudio never blocks.
  bool hazardous;
  do {
    hazardous = false;
    for (auto& hazard : mixHazards) {
      if (hazard.load(std::memory_order_seq_cst) == removedState) {
        hazardous = true;
        std::this_thread::yield();
        break;
      }
    }
  } while (hazardous);

  auto i = std::find(boundThreads.begin(), boundThreads.end(), other);
  if (i != boundThreads.end()) {
    boundThreads.erase(i);
  }
  mixStateOwners.erase(
      std::remove(mixStateOwners.begin(), mixStateOwners.end(), other->mixState),
      mixStateOwners.end());
}

void AudioThread::deviceCleanup() {
  //
  //NOT PROTECTED by m_device_mutex on purpose, to prevent deadlocks with i->second->controllerThread
  // it doesn't matter, it is only called when all "normal" audio threads are detached from the controller.
  //
  for (auto& i : deviceController) {

    delete i.second;
  }

  deviceController.clear();
}

static int audioCallback(void* outputBuffer, void* /* inputBuffer */, unsigned int nBufferFrames, double /* streamTime */, RtAudioStreamStatus status,
                         void* userData) {
  float* out = (float*)outputBuffer;

  // Zero output in all cases so inactive or starved demodulators are silent.
  ::memset(out, 0, nBufferFrames * 2 * sizeof(float));

  auto* src = (AudioThread*)userData;
  if (src->isTerminated()) {
    return 1;
  }

  if (status) {
    src->underflowCount.fetch_add(1, std::memory_order_relaxed);
  }

  for (size_t sourceIndex = 0; sourceIndex < AudioThread::MAX_MIX_SOURCES; ++sourceIndex) {
    AudioMixState* state = src->protectMixSource(sourceIndex);
    if (!state || !state->active.load(std::memory_order_acquire)) {
      src->releaseMixSource(sourceIndex);
      continue;
    }
    if (!state->primed.load(std::memory_order_acquire)) {
      // Hold roughly 20 ms before starting/restarting. The SDR can deliver
      // audio in larger, less frequent packets than CoreAudio requests; a
      // half-callback threshold repeatedly drained the ring between packets.
      const int outputRate = state->sampleRate.load(std::memory_order_relaxed);
      const size_t jitterFrames = outputRate > 0
          ? static_cast<size_t>(outputRate / 50)
          : 1;
      const size_t startupFrames = std::max<size_t>(nBufferFrames * 2, jitterFrames);
      if (state->ring.available() < startupFrames) {
        src->releaseMixSource(sourceIndex);
        continue;
      }
      state->primed.store(true, std::memory_order_release);
    }

    const float sourceGain = state->gain.load(std::memory_order_relaxed);
    for (unsigned int i = 0; i < nBufferFrames; ++i) {
      float left = 0.0f, right = 0.0f;
      if (!state->ring.pop(left, right)) {
        state->underflows.fetch_add(1, std::memory_order_relaxed);
        state->primed.store(false, std::memory_order_release);
        break;
      }
      out[i * 2] += left * sourceGain;
      out[i * 2 + 1] += right * sourceGain;
    }
    src->releaseMixSource(sourceIndex);
  }

  float peak = 0.0f;
  for (unsigned int i = 0; i < nBufferFrames * 2; ++i) peak = std::max(peak, std::fabs(out[i]));
  if (peak > 1.0f) {
    const float invPeak = 1.0f / peak;

    for (unsigned int i = 0; i < nBufferFrames * 2; i++) {
      out[i] *= invPeak;
    }
  }

  return 0;
}

void AudioThread::enumerateDevices(std::vector<RtAudio::DeviceInfo>& devs) {
  RtAudio endac;

  unsigned int numDevices = endac.getDeviceCount();

  for (unsigned int i = 0; i < numDevices; i++) {
    RtAudio::DeviceInfo info = endac.getDeviceInfo(i);

    devs.push_back(info);

    std::cout << std::endl;

    std::cout << "Audio Device #" << i << " " << info.name << std::endl;
    std::cout << "\tDefault Output? " << (info.isDefaultOutput ? "Yes" : "No") << std::endl;
    std::cout << "\tDefault Input? " << (info.isDefaultInput ? "Yes" : "No") << std::endl;
    std::cout << "\tInput channels: " << info.inputChannels << std::endl;
    std::cout << "\tOutput channels: " << info.outputChannels << std::endl;
    std::cout << "\tDuplex channels: " << info.duplexChannels << std::endl;

    std::cout << "\t" << "Native formats:" << std::endl;
    RtAudioFormat nFormats = info.nativeFormats;
    if (nFormats & RTAUDIO_SINT8) {
      std::cout << "\t\t8-bit signed integer." << std::endl;
    }
    if (nFormats & RTAUDIO_SINT16) {
      std::cout << "\t\t16-bit signed integer." << std::endl;
    }
    if (nFormats & RTAUDIO_SINT24) {
      std::cout << "\t\t24-bit signed integer." << std::endl;
    }
    if (nFormats & RTAUDIO_SINT32) {
      std::cout << "\t\t32-bit signed integer." << std::endl;
    }
    if (nFormats & RTAUDIO_FLOAT32) {
      std::cout << "\t\t32-bit float normalized between plus/minus 1.0." << std::endl;
    }
    if (nFormats & RTAUDIO_FLOAT64) {
      std::cout << "\t\t64-bit float normalized between plus/minus 1.0." << std::endl;
    }

    std::vector<unsigned int>::iterator srate;

    std::cout << "\t" << "Supported sample rates:" << std::endl;

    for (srate = info.sampleRates.begin(); srate != info.sampleRates.end(); srate++) {
      std::cout << "\t\t" << (*srate) << "hz" << std::endl;
    }

    std::cout << std::endl;
  }
}

void AudioThread::setDeviceSampleRate(int deviceId, int sampleRate) {

  AudioThread* matchingControllerThread = nullptr;

  //scope lock here to minimize the common unique static lock contention
  {
    std::lock_guard<std::recursive_mutex> lock(m_device_mutex);

    if (deviceController.find(deviceId) != deviceController.end()) {

      matchingControllerThread = deviceController[deviceId];
    }
  }

  //out-of-lock test
  if (matchingControllerThread != nullptr) {

    AudioThreadCommand refreshDevice;
    refreshDevice.cmdType = AudioThreadCommand::Type::AUDIO_THREAD_CMD_SET_SAMPLE_RATE;
    refreshDevice.int_value = sampleRate;
    //VSO : blocking push !
    matchingControllerThread->getCommandQueue()->push(refreshDevice);
  }
}

void AudioThread::setSampleRate(int sampleRate_in) {

  bool thisIsAController = false;
  bool resetDenoise = false;

  //scope lock here to minimize the common unique static lock contention
  {
    std::lock_guard<std::recursive_mutex> lock(m_device_mutex);

    const auto controller = deviceController.find(outputDevice.load());
    if (controller != deviceController.end() && controller->second == this) {
      thisIsAController = true;
      deviceSampleRate[outputDevice.load()] = sampleRate_in;
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    resetDenoise = !thisIsAController && sampleRate != sampleRate_in;

    if (thisIsAController) {
      if (dac.isStreamOpen()) {
        if (dac.isStreamRunning()) dac.stopStream();
        dac.closeStream();
      }

      //Set bounded sample rate:
      for (auto srcmix : boundThreads) {
        srcmix->setSampleRate(sampleRate_in);
      }

      //make a local copy, snapshot of the list of demodulators
      std::vector<DemodulatorInstancePtr> demodulators = wxGetApp().getDemodMgr().getDemodulators();

      for (const auto& demod : demodulators) {
        if (demod->getOutputDevice() == outputDevice.load()) {
          demod->setAudioSampleRate(sampleRate_in);
        }
      }

      try {
        dac.openStream(&parameters, nullptr, RTAUDIO_FLOAT32, sampleRate_in, &nBufferFrames, &audioCallback, (void*)this, &opts);
        dac.startStream();
      } catch (RtAudioError& e) {
        e.printMessage();
        active.store(false);
        mixState->active.store(false, std::memory_order_release);
      }
    }

    sampleRate = sampleRate_in;
    mixState->sampleRate.store(sampleRate_in, std::memory_order_release);
    mixState->clear();
  }

  if (resetDenoise) {
    setDenoiseEnabled(denoiseEnabled.load());
  }
}

int AudioThread::getSampleRate() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  return sampleRate;
}

void AudioThread::setupDevice(int deviceId) {

  //global lock to setup the device...
  std::lock_guard<std::recursive_mutex> lock(m_device_mutex);

  parameters.deviceId = deviceId;
  parameters.nChannels = 2;
  parameters.firstChannel = 0;

  opts.streamName = "CubicSDR Audio Output";

  try {
    if (deviceController.find(outputDevice.load()) != deviceController.end()) {
      //'this' is not the controller, so remove it from the bounded list:
      //beware, we must take the controller mutex, because the audio callback may use the list of bounded
      //threads at that moment:
      std::lock_guard<std::recursive_mutex> lock(deviceController[outputDevice.load()]->getMutex());

      deviceController[outputDevice.load()]->removeThread(this);
    }
#ifndef _MSC_VER
    opts.priority = sched_get_priority_max(SCHED_FIFO);
#endif
    //    opts.flags = RTAUDIO_MINIMIZE_LATENCY;
    opts.flags = RTAUDIO_SCHEDULE_REALTIME;

    if (deviceSampleRate.find(parameters.deviceId) != deviceSampleRate.end()) {
      sampleRate = deviceSampleRate[parameters.deviceId];
    } else {
      std::cout << "Error, device sample rate wasn't initialized?" << std::endl;
      return;
      //            sampleRate = AudioThread::getDefaultAudioSampleRate();
      //            deviceSampleRate[parameters.deviceId] = sampleRate;
    }

    //Create a new controller:
    if (deviceController.find(parameters.deviceId) == deviceController.end()) {

      //Create a new controller thread for parameters.deviceId:
      auto* newController = new AudioThread();

      newController->setInitOutputDevice(parameters.deviceId, sampleRate);
      newController->bindThread(this);
      newController->attachControllerThread(new std::thread(&AudioThread::threadMain, newController));

      deviceController[parameters.deviceId] = newController;
    } else if (deviceController[parameters.deviceId] == this) {

      //Attach callback
      dac.openStream(&parameters, nullptr, RTAUDIO_FLOAT32, sampleRate, &nBufferFrames, &audioCallback, (void*)this, &opts);
      dac.startStream();
    } else {
      //we are a bound thread, add ourselves to the controller deviceController[parameters.deviceId].
      //beware, we must take the controller mutex, because the audio callback may use the list of bounded
      //threads at that moment:
      std::lock_guard<std::recursive_mutex> lock(deviceController[parameters.deviceId]->getMutex());

      deviceController[parameters.deviceId]->bindThread(this);
    }
    active = true;
    mixState->sampleRate.store(sampleRate, std::memory_order_release);
    mixState->active.store(true, std::memory_order_release);
    mixState->clear();

  } catch (RtAudioError& e) {
    e.printMessage();
    return;
  }
  if (deviceId != -1) {
    outputDevice = deviceId;
  }
}

int AudioThread::getOutputDevice() {

  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  if (outputDevice == -1) {
    return dac.getDefaultOutputDevice();
  }
  return outputDevice;
}

void AudioThread::setInitOutputDevice(int deviceId, int sampleRate_in) {

  //global lock
  std::lock_guard<std::recursive_mutex> lock(m_device_mutex);

  outputDevice = deviceId;
  if (sampleRate_in == -1) {
    if (deviceSampleRate.find(deviceId) != deviceSampleRate.end()) {
      sampleRate_in = deviceSampleRate[deviceId];
    }
  } else {
    deviceSampleRate[deviceId] = sampleRate_in;
  }
  sampleRate = sampleRate_in;
  mixState->sampleRate.store(sampleRate_in, std::memory_order_release);
  mixState->clear();
}

void AudioThread::run() {
#ifdef __APPLE__
  // macOS ignores portable realtime policies for ordinary application
  // threads. QoS is the supported scheduler hint for latency-sensitive work.
  if (__builtin_available(macOS 10.10, *)) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  }
#endif

  //    std::cout << "Audio thread initializing.." << std::endl;

  if (dac.getDeviceCount() < 1) {
    std::cout << "No audio devices found!" << std::endl;
    return;
  }

  inputQueue = std::static_pointer_cast<AudioThreadInputQueue>(getInputQueue("AudioDataInput"));

  setupDevice((outputDevice.load() == -1) ? (dac.getDefaultOutputDevice()) : outputDevice.load());

  //    std::cout << "Audio thread started." << std::endl;

  auto handleCommand = [this](const AudioThreadCommand& command) {
    if (command.cmdType == AudioThreadCommand::Type::AUDIO_THREAD_CMD_SET_DEVICE) {
      setupDevice(command.int_value);
    } else if (command.cmdType == AudioThreadCommand::Type::AUDIO_THREAD_CMD_SET_SAMPLE_RATE) {
      setSampleRate(command.int_value);
    }
  };

  // Bound audio threads prepare playback data here. The controller has no
  // input queue and only services output-device commands.
  while (!stopping) {
    AudioThreadCommand command;
    while (cmdQueue.try_pop(command)) {
      handleCommand(command);
    }

    if (inputQueue) {
      AudioThreadInputPtr input;
      if (inputQueue->pop(input, 5000)) {
        processAudioInput(input);
      }
    } else if (cmdQueue.pop(command, HEARTBEAT_CHECK_PERIOD_MICROS)) {
      handleCommand(command);
    }
  }  //end while

  // Drain any remaining inputs, with a non-blocking pop
  if (inputQueue != nullptr) {
    inputQueue->flush();
  }
  mixState->active.store(false, std::memory_order_release);
  mixState->clear();

  //Stop : Retrieve the matching controlling thread in a scope lock:
  AudioThread* controllerMatchingThread;
  {
    std::lock_guard<std::recursive_mutex> global_lock(m_device_mutex);
    const auto controller = deviceController.find(parameters.deviceId);
    controllerMatchingThread = controller == deviceController.end() ? nullptr : controller->second;
  }

  if (controllerMatchingThread && controllerMatchingThread != this) {
    //'this' is not the controller, so remove it from the bounded list:
    //beware, we must take the controller mutex, because the audio callback may use the list of bounded
    //threads at that moment:
    std::lock_guard<std::recursive_mutex> lock(controllerMatchingThread->getMutex());

    controllerMatchingThread->removeThread(this);
  } else {
    // 'this' is a controller thread:
    try {
      if (dac.isStreamOpen()) {
        dac.stopStream();
      }
      dac.closeStream();

    } catch (RtAudioError& e) {
      e.printMessage();
    }
  }

  //    std::cout << "Audio thread done." << std::endl;
}

void AudioThread::terminate() {
  IOThread::terminate();
  denoiseEnabled.store(false);

  {
    std::lock_guard<std::mutex> lock(denoiseMutex);
    resetDenoiseProcessorLocked();
  }

  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (inputQueue) {
    inputQueue->flush();
  }
  mixState->active.store(false, std::memory_order_release);
  mixState->clear();
}

bool AudioThread::isActive() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);

  return active;
}

void AudioThread::setActive(bool state) {

  AudioThread* matchingControllerThread = nullptr;

  //scope lock here to minimize the common unique static lock contention
  {
    std::lock_guard<std::recursive_mutex> lock(m_device_mutex);

    if (deviceController.find(parameters.deviceId) != deviceController.end()) {

      matchingControllerThread = deviceController[parameters.deviceId];
    }
  }

  if (matchingControllerThread == nullptr) {
    return;
  }

  const bool wasActive = active.load();

  // Never take the controller lock while holding this bound thread's lock:
  // audioCallback takes them in the opposite (controller -> bound) order.
  if (state && !wasActive && inputQueue) {
    matchingControllerThread->bindThread(this);
  }

  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Activity state changing, clear any inputs.
    if (inputQueue) {
      inputQueue->flush();
    }
    active = state;
    mixState->active.store(state, std::memory_order_release);
    mixState->clear();
  }

  if (!state && wasActive) {
    matchingControllerThread->removeThread(this);
  }
}

AudioThreadCommandQueue* AudioThread::getCommandQueue() {
  return &cmdQueue;
}

void AudioThread::setGain(float gain_in) {

  if (gain_in < 0.0) {
    gain_in = 0.0;
  }
  if (gain_in > 2.0) {
    gain_in = 2.0;
  }
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  mixState->gain.store(gain_in, std::memory_order_release);
}

void AudioThread::setDenoiseEnabled(bool enabled) {
  denoiseEnabled.store(enabled);

  {
    std::lock_guard<std::mutex> lock(denoiseMutex);
    resetDenoiseProcessorLocked();
    if (enabled) {
      denoiseState = rnnoise_create(nullptr);
    }
  }

  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (inputQueue) {
    inputQueue->flush();
  }
  mixState->clear();
}

void AudioThread::resetDenoiseProcessorLocked() {
  if (denoiseState != nullptr) {
    rnnoise_destroy(denoiseState);
    denoiseState = nullptr;
  }
  if (denoiseInputResampler != nullptr) {
    msresamp_rrrf_destroy(denoiseInputResampler);
    denoiseInputResampler = nullptr;
  }
  if (denoiseOutputResampler != nullptr) {
    msresamp_rrrf_destroy(denoiseOutputResampler);
    denoiseOutputResampler = nullptr;
  }
  denoiseFrame.fill(0.0f);
  denoiseFrameFill = 0;
  denoiseProcessorSampleRate = 0;
  denoiseModelInput.clear();
  denoiseModelOutput.clear();
}

void AudioThread::queuePlaybackInput(const AudioThreadInputPtr& input) {
  if (!input) {
    return;
  }

  mixState->queue(*input);
}

void AudioThread::processAudioInput(const AudioThreadInputPtr& input) {
  if (!input) {
    return;
  }

  if (!denoiseEnabled.load() || input->channels != 1) {
    queuePlaybackInput(input);
    return;
  }

  if (input->sampleRate <= 0 || input->data.empty()) {
    queuePlaybackInput(input);
    return;
  }

  std::lock_guard<std::mutex> lock(denoiseMutex);
  if (!denoiseEnabled.load()) {
    queuePlaybackInput(input);
    return;
  }
  if (input->discontinuity) {
    // Do not combine a partial RNNoise frame or resampler history with audio
    // from the far side of a dropped IQ block.
    resetDenoiseProcessorLocked();
  }
  // RNNoise is fixed at 480 samples/48 kHz. Keep streaming resamplers on both
  // sides so changing CubicSDR's audio rate does not disable denoising.
  if (denoiseProcessorSampleRate != input->sampleRate) {
    resetDenoiseProcessorLocked();
    denoiseProcessorSampleRate = input->sampleRate;
    denoiseState = rnnoise_create(nullptr);
    if (input->sampleRate != 48000) {
      constexpr float stopBandAttenuation = 60.0f;
      denoiseInputResampler = msresamp_rrrf_create(48000.0f / input->sampleRate, stopBandAttenuation);
      denoiseOutputResampler = msresamp_rrrf_create(input->sampleRate / 48000.0f, stopBandAttenuation);
    }
  }

  if (denoiseState == nullptr ||
      (input->sampleRate != 48000 &&
       (denoiseInputResampler == nullptr || denoiseOutputResampler == nullptr))) {
    queuePlaybackInput(input);
    return;
  }

  float* modelInput = input->data.data();
  size_t modelInputSize = input->data.size();
  if (denoiseInputResampler != nullptr) {
    const unsigned int outputCapacity =
        msresamp_rrrf_get_num_output(denoiseInputResampler, static_cast<unsigned int>(input->data.size()));
    denoiseModelInput.resize(outputCapacity);
    unsigned int samplesWritten = 0;
    msresamp_rrrf_execute(denoiseInputResampler,
                          input->data.data(),
                          static_cast<unsigned int>(input->data.size()),
                          denoiseModelInput.data(),
                          &samplesWritten);
    denoiseModelInput.resize(samplesWritten);
    modelInput = denoiseModelInput.data();
    modelInputSize = denoiseModelInput.size();
  }

  AudioThreadInputPtr output = denoiseOutputBuffers.getBuffer();
  output->frequency = input->frequency;
  output->inputRate = input->inputRate;
  output->sampleRate = input->sampleRate;
  output->channels = 1;
  output->type = input->type;
  output->is_squelch_active = input->is_squelch_active;
  output->is_denoised = true;
  output->discontinuity = input->discontinuity;
  output->data.clear();
  output->dataOut.clear();

  const size_t completedFrames = (denoiseFrameFill + modelInputSize) / RNNOISE_FRAME_SIZE;
  std::vector<float>* modelOutput = &output->data;
  if (denoiseOutputResampler != nullptr) {
    denoiseModelOutput.clear();
    modelOutput = &denoiseModelOutput;
  }
  modelOutput->reserve(completedFrames * RNNOISE_FRAME_SIZE);

  for (size_t i = 0; i < modelInputSize; i++) {
    denoiseFrame[denoiseFrameFill++] = modelInput[i] * 32767.0f;
    if (denoiseFrameFill == RNNOISE_FRAME_SIZE) {
      rnnoise_process_frame(denoiseState, denoiseFrame.data(), denoiseFrame.data());
      for (float denoisedSample : denoiseFrame) {
        modelOutput->push_back(denoisedSample / 32767.0f);
      }
      denoiseFrameFill = 0;
    }
  }

  if (modelOutput->empty()) {
    return;
  }

  if (denoiseOutputResampler != nullptr) {
    const unsigned int outputCapacity =
        msresamp_rrrf_get_num_output(denoiseOutputResampler, static_cast<unsigned int>(modelOutput->size()));
    output->data.resize(outputCapacity);
    unsigned int samplesWritten = 0;
    msresamp_rrrf_execute(denoiseOutputResampler,
                          modelOutput->data(),
                          static_cast<unsigned int>(modelOutput->size()),
                          output->data.data(),
                          &samplesWritten);
    output->data.resize(samplesWritten);
    if (output->data.empty()) {
      return;
    }
  }

  output->peak = 0.0f;
  for (float sample : output->data) {
    output->peak = std::max(output->peak, std::abs(sample));
  }
  queuePlaybackInput(output);
}
