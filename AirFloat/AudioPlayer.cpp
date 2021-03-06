//
//  AudioPlayer.cpp
//  AirFloat
//
//  Created by Kristian Trenskow on 1/21/12.
//  Copyright (c) 2012 The Famous Software Company. All rights reserved.
//

#include <QuartzCore/QuartzCore.h>

#include "CAHostTimeBase.h"
#include "CAXException.h"
#include "Log.h"

#include "AppleLosslessSoftwareAudioConverter.h"
#include "AppleLosslessAudioConverter.h"

#include "Settings.h"

#include "AudioPlayer.h"

#ifndef MIN
#define MIN(x,y) (x < y ? x : y)
#endif
#define SILENT_PACKET_SIZE (_srcFramesPerPacket * _audio.outDesc.mBytesPerFrame * 2)
#define MAX_PACKET_SIZE ((double)SILENT_PACKET_SIZE * 1.1)
#define FROM_CLIENT_TIME(x) (x + _clientServerDifference)
#define TO_CLIENT_TIME(x) (x - _clientServerDifference)
#define FROM_DEVICE_TIME(x) (CAHostTimeBase::ConvertToNanos(x) / 1000000000.0)
#define TO_DEVICE_TIME(x) CAHostTimeBase::ConvertFromNanos(x * 1000000000.0)
#define ACCEPTABLE_DELAY 0.01

AudioPlayer::AudioPlayer(int* fmts, int fmtsSize) {
    
    assert(fmts != NULL && fmtsSize > 0);
    
#if TARGET_OS_IPHONE
    UInt32 category = kAudioSessionCategory_MediaPlayback;
    AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(category), &category);
    AudioSessionSetActive(true);
#endif
        
    bzero(&_audio, sizeof(_audio));
    
    _audio.losslessConverter = new AppleLosslessAudioConverter(fmts, fmtsSize);
    _audio.speedUpConverter = new AudioConverter(fmts[10], fmts[10] - 441);
    _audio.slowDownConverter = new AudioConverter(fmts[10], fmts[10] + 441);
    
    _srcFramesPerPacket = fmts[0];
    
    _clientServerDifference = 0;
    _nextPacketTime = 0;
    _outputIsHomed = false;
    
    bzero(_clientServerDifferenceHistory, sizeof(double) * CLIENT_SERVER_DIFFERENCE_BACKLOG);
    _clientServerDifferenceHistoryCount = 0;
        
    _audio.outDesc = _audio.losslessConverter->getDestDescription();

    _audioQueue = new AudioQueue(fmts[0], _audio.outDesc.mBytesPerFrame, _audio.outDesc.mSampleRate);
    
    XThrowIfError(
                  AudioQueueNewOutput(&_audio.outDesc, AudioPlayer::_audioOutputCallbackHelper, this, NULL, NULL, 0, &_audio.queue);
                  , "AudioQueueNewOutput");
    
    XThrowIfError(
                  AudioQueueCreateTimeline(_audio.queue, &_audio.timeline)
                  , "AudioQueueCreateTimeline");
    
    for (int i = 0 ; i < BUFFER_COUNT ; i++) {
        AudioQueueAllocateBuffer(_audio.queue, MAX_PACKET_SIZE, &_audio.buffers[i]);
        _audio.buffers[i]->mAudioDataByteSize = SILENT_PACKET_SIZE;
        bzero(_audio.buffers[i]->mAudioData, MAX_PACKET_SIZE);
        AudioQueueEnqueueBuffer(_audio.queue, _audio.buffers[i], 0, NULL);
    }
    
    _preFlushVolume = 1.0;
    _flushedSeq = -1;
    
    _skipPacket = false;
    
}

AudioPlayer::~AudioPlayer() {
    
    AudioQueueStop(_audio.queue, true);
    
    for (int i = 0 ; i < BUFFER_COUNT ; i++)
        AudioQueueFreeBuffer(_audio.queue, _audio.buffers[i]);
    
    AudioQueueDispose(_audio.queue, true);
    
    delete _audioQueue;
    
    delete _audio.losslessConverter;
    delete _audio.speedUpConverter;
    delete _audio.slowDownConverter;
    
}

void AudioPlayer::start() {
    
    OSStatus err;
    if (noErr == (err = AudioQueueStart(_audio.queue, NULL)))
        log(LOG_INFO, "Audio queue started.");
    XThrowIfError(
                  err
                  , "AudioQueueStart");
    
}

void AudioPlayer::flush(int lastSeq) {
    
    _timeMutex.lock();
    
    _packetMutex.lock();
    
    _audioQueue->_flush();
    _flushedSeq = lastSeq;
    
    _packetMutex.unlock();
    
    AudioQueueSetParameter(_audio.queue, kAudioQueueParam_Volume, 0.0);
    
    _outputIsHomed = false;
    _nextPacketTime = 0;
    
    _timeMutex.unlock();
        
}

void AudioPlayer::setVolume(double volume) {
    
    _preFlushVolume = volume;
    AudioQueueSetParameter(_audio.queue, kAudioQueueParam_Volume, _preFlushVolume);
    
}

void AudioPlayer::setClientTime(double time) {
    
    _timeMutex.lock();
    
    AudioTimeStamp timeStamp;
    bzero(&timeStamp, sizeof(AudioTimeStamp));
    AudioQueueDeviceGetCurrentTime(_audio.queue, &timeStamp);
    
    _clientServerDifference = 0;
    
    _clientServerDifferenceHistoryCount = MIN(_clientServerDifferenceHistoryCount + 1, CLIENT_SERVER_DIFFERENCE_BACKLOG);
    for (int i = _clientServerDifferenceHistoryCount - 1 ; i > 0 ; i--)
        _clientServerDifference += (_clientServerDifferenceHistory[i] = _clientServerDifferenceHistory[i-1]);
    
    _clientServerDifference += (_clientServerDifferenceHistory[0] = CAHostTimeBase::ConvertToNanos(timeStamp.mHostTime) / 1000000000.0 - time);
    
    _clientServerDifference /= (double)_clientServerDifferenceHistoryCount;
    
    log(LOG_INFO, "Time difference: %1.5f", _clientServerDifference);
    
    _timeMutex.unlock();
    
}

AudioQueue* AudioPlayer::getAudioQueue() {
    
    return _audioQueue;
    
}

void AudioPlayer::_audioOutputCallback(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    
    UInt32 size = sizeof(UInt32);
    UInt32 isRunning = 0;
    AudioQueueGetProperty(_audio.queue, kAudioQueueProperty_IsRunning, &isRunning, &size);
    
    if (!isRunning)
        return;
    
    _timeMutex.lock();
    
    inBuffer->mAudioDataByteSize = SILENT_PACKET_SIZE;
    bzero(inBuffer->mAudioData, SILENT_PACKET_SIZE);
    
    double time = 0;
    uint32_t sampleTime = 0;
    bool hasPacket = _audioQueue->hasAvailablePacket();
    
    if (_nextPacketTime > 0 && hasPacket) {
        
        if (!_outputIsHomed) {
            
            double timeToStart = (FROM_CLIENT_TIME((time = _audioQueue->getPacketTime())) - _nextPacketTime);

            inBuffer->mAudioDataByteSize = MIN((timeToStart * _audio.outDesc.mSampleRate * _audio.outDesc.mBytesPerPacket), SILENT_PACKET_SIZE);
            
            if (timeToStart - ((_srcFramesPerPacket * 2) / _audio.outDesc.mSampleRate) <= 0) {
                _outputIsHomed = true;
                log(LOG_INFO, "Output is homed (client time: %1.6f)", TO_CLIENT_TIME(CACurrentMediaTime()));
            }
            
            time = 0;

        } else {
            
            AudioQueueSetParameter(_audio.queue, kAudioQueueParam_Volume, _preFlushVolume);
            
            if (!_skipPacket) {
                inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
                _audioQueue->getPacket(inBuffer->mAudioData, (uint32_t*)&inBuffer->mAudioDataByteSize, &time, &sampleTime);
            }
                            
        }
        
    } else if (_nextPacketTime > 0 && hasPacket == false && _outputIsHomed) {
        _outputIsHomed = false;
        _skipPacket = false;
        log(LOG_INFO, "Output lost synchronization");
    }
    
    AudioTimeStamp timeStamp;
    
    if (noErr == AudioQueueEnqueueBufferWithParameters(_audio.queue, inBuffer, 0, NULL, 0, 0, 0, NULL, NULL, &timeStamp)) {
        
        double currentPacketTime = CAHostTimeBase::ConvertToNanos(timeStamp.mHostTime) / 1000000000.0;
        _nextPacketTime = currentPacketTime + ((inBuffer->mAudioDataByteSize / _audio.outDesc.mBytesPerPacket) / _audio.outDesc.mSampleRate);
        
        if (Settings::isSyncronizationEnabled()) {
            
            if (time > 0 && _outputIsHomed && !_skipPacket) {
                
                double audioDelay = (CAHostTimeBase::ConvertToNanos(timeStamp.mHostTime) / 1000000000.0) - (time + _clientServerDifference);
                if (audioDelay > (double)_srcFramesPerPacket / _audio.outDesc.mSampleRate) {
                    _audioQueue->discardPacket();
                    log(LOG_INFO, "Skipped packet to keep up with syncronization");
                } else if (-audioDelay > (double)_srcFramesPerPacket / _audio.outDesc.mSampleRate)
                    _skipPacket = true;
                
            } else
                _skipPacket = false;

        }
        
    } else
        log(LOG_ERROR, "Cannot enqueue audio buffer");
    
    _timeMutex.unlock();
    
}

void AudioPlayer::_audioOutputCallbackHelper(void* aqData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    
    assert(aqData != NULL);
    
    ((AudioPlayer*)aqData)->_audioOutputCallback(inAQ, inBuffer);
    
}

int AudioPlayer::addAudio(void* buffer, uint32_t size, int seqNo, uint32_t sampleTime) {
    
    _packetMutex.lock();
    if (seqNo <= _flushedSeq) {
        log(LOG_INFO, "Audio packet (seq %d) ignored", seqNo);
        _packetMutex.unlock();
        return 0;
    } else
        _flushedSeq = -1;
    _packetMutex.unlock();
    
    int ret = 0;
    
    uint32_t outsize = _srcFramesPerPacket * _audio.outDesc.mBytesPerFrame;
    void* rawBuffer = malloc(outsize);
    
    _audio.losslessConverter->convert(buffer, size, rawBuffer, &outsize);
    if (outsize == 0)
        log(LOG_ERROR, "DECODING ERROR!");
    
    if (size > 0)
        ret = _audioQueue->_addAudioPacket(rawBuffer, outsize, seqNo, sampleTime);
    
    free(rawBuffer);
    
    return ret;
    
}

double AudioPlayer::getSampleRate() {
    
    return _audio.outDesc.mSampleRate;
    
}