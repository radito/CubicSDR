// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "AudioSinkThread.h"

#define HEARTBEAT_CHECK_PERIOD_MICROS (50 * 1000) 

AudioSinkThread::AudioSinkThread() {
    inputQueuePtr = std::make_shared<AudioThreadInputQueue>();
    inputQueuePtr->set_max_num_items(1000);
    setInputQueue("input", inputQueuePtr);
}

AudioSinkThread::~AudioSinkThread() = default;

void AudioSinkThread::run() {
#ifdef __APPLE__
    if (__builtin_available(macOS 10.10, *)) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    }
#endif

    AudioThreadInputPtr inp;
    AudioThreadInput inputRef;

    while (!stopping) {
        if (!inputQueuePtr->pop(inp, HEARTBEAT_CHECK_PERIOD_MICROS)) {
            continue;
        }

        if (inputRef.channels != inp->channels || 
                inputRef.frequency != inp->frequency ||
                inputRef.inputRate != inp->inputRate ||
                inputRef.sampleRate != inp->sampleRate) {

            inputChanged(inputRef, inp);

            inputRef.channels = inp->channels;
            inputRef.frequency = inp->frequency;
            inputRef.inputRate = inp->inputRate;
            inputRef.sampleRate = inp->sampleRate;
        }

        sink(inp);
    }
}

void AudioSinkThread::terminate() {
    IOThread::terminate();
    inputQueuePtr->flush();
}
