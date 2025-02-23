/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "backends/mixer/atari/atari-mixer.h"

#include <math.h>
#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>

#include "common/config-manager.h"
#include "common/debug.h"

// see https://github.com/mikrosk/atari_sound_setup
#include "../../../../atari_sound_setup.git/atari_sound_setup.h"

#define DEFAULT_OUTPUT_RATE 24585
#define DEFAULT_SAMPLES 2048	// 83ms

void AtariAudioShutdown() {
	AtariSoundSetupDeinitXbios();
}

AtariMixerManager::AtariMixerManager() : MixerManager() {
	debug("AtariMixerManager()");

	_audioSuspended = true;

	ConfMan.registerDefault("output_rate", DEFAULT_OUTPUT_RATE);

	_outputRate = ConfMan.getInt("output_rate");
	if (_outputRate <= 0)
		_outputRate = DEFAULT_OUTPUT_RATE;

	g_system->getEventManager()->getEventDispatcher()->registerObserver(this, 10, false);
}

AtariMixerManager::~AtariMixerManager() {
	debug("~AtariMixerManager()");

	g_system->getEventManager()->getEventDispatcher()->unregisterObserver(this);

	AtariAudioShutdown();

	Mfree(_atariSampleBuffer);
	_atariSampleBuffer = _atariPhysicalSampleBuffer = _atariLogicalSampleBuffer = nullptr;

	delete[] _samplesBuf;
}

void AtariMixerManager::init() {
	AudioSpec desired, obtained;

	desired.frequency = _outputRate;
	desired.channels = 2;
	desired.format = AudioFormatSigned16MSB;
	desired.samples = DEFAULT_SAMPLES;

	if (!AtariSoundSetupInitXbios(&desired, &obtained)) {
		error("Sound system is not available");
	}

	if (obtained.channels != 2 && obtained.format != AudioFormatSigned16MSB) {
		error("Sound system currently supports only 16-bit signed stereo samples (big endian)");
	}

	_outputRate = obtained.frequency;

	ConfMan.setInt("output_rate", _outputRate);
	debug("setting %d Hz mixing frequency", _outputRate);

	_samples = 8192;
	while (_samples * 16 > _outputRate * 2)
		_samples >>= 1;

	ConfMan.registerDefault("audio_buffer_size", (int)_samples);

	int samples = ConfMan.getInt("audio_buffer_size");
	if (samples > 0)
		_samples = samples;

	ConfMan.setInt("audio_buffer_size", (int)_samples);
	debug("sample buffer size: %d", _samples);

	ConfMan.flushToDisk();

	_atariSampleBufferSize = _samples * 4;

	_atariSampleBuffer = (byte*)Mxalloc(_atariSampleBufferSize * 2, MX_STRAM);
	if (!_atariSampleBuffer)
		error("Failed to allocate memory in ST RAM");

	_atariPhysicalSampleBuffer = _atariSampleBuffer;
	_atariLogicalSampleBuffer = _atariSampleBuffer + _atariSampleBufferSize;

	Setbuffer(SR_PLAY, _atariSampleBuffer, _atariSampleBuffer + 2 * _atariSampleBufferSize);

	_samplesBuf = new uint8[_samples * 4];

	_mixer = new Audio::MixerImpl(_outputRate, _samples);
	_mixer->setReady(true);

	_audioSuspended = false;
}

void AtariMixerManager::suspendAudio() {
	debug("suspendAudio");

	stopPlayback(kPlaybackStopped);

	_audioSuspended = true;
}

int AtariMixerManager::resumeAudio() {
	debug("resumeAudio");

	update();

	_audioSuspended = false;
	return 0;
}

bool AtariMixerManager::notifyEvent(const Common::Event &event) {
	switch (event.type) {
	case Common::EVENT_QUIT:
	case Common::EVENT_RETURN_TO_LAUNCHER:
		stopPlayback(kPlaybackStopped);
		debug("silencing the mixer");
		return false;
	default:
		break;
	}

	return false;
}

void AtariMixerManager::startPlayback(PlaybackState playbackState) {
	Buffoper(SB_PLA_ENA | SB_PLA_RPT);
	_playbackState = playbackState;
	debug("playback started");
}

void AtariMixerManager::stopPlayback(PlaybackState playbackState) {
	Buffoper(0x00);
	_playbackState = playbackState;
	debug("playback stopped");
}

void AtariMixerManager::update() {
	if (_audioSuspended) {
		return;
	}

	assert(_mixer);

	uint32 processed = 0;
	if (_playbackState == kPlaybackStopped) {
		// playback stopped means that we are not playing anything (DMA
		// pointer is not updating) but the mixer may have something available
		processed = _mixer->mixCallback(_samplesBuf, _samples * 4);

		if (processed > 0) {
			memcpy(_atariPhysicalSampleBuffer, _samplesBuf, processed * 4);
			memset(_atariPhysicalSampleBuffer + processed, 0, (_samples - processed) * 4);
			startPlayback(kPlayingFromPhysicalBuffer);
		}
	} else {
		SndBufPtr sPtr;
		if (Buffptr(&sPtr) != 0) {
			warning("Buffptr() failed");
			return;
		}

		byte *buf = nullptr;

		if (_playbackState == kPlayingFromPhysicalBuffer) {
			if ((byte*)sPtr.play < _atariLogicalSampleBuffer) {
				buf = _atariLogicalSampleBuffer;
				_playbackState = kPlayingFromLogicalBuffer;
			}
		} else if (_playbackState == kPlayingFromLogicalBuffer) {
			if ((byte*)sPtr.play >= _atariLogicalSampleBuffer) {
				buf = _atariPhysicalSampleBuffer;
				_playbackState = kPlayingFromPhysicalBuffer;
			}
		}

		if (buf) {
			processed = _mixer->mixCallback(_samplesBuf, _samples * 4);
			if (processed > 0) {
				memcpy(buf, _samplesBuf, processed * 4);
			} else {
				stopPlayback(kPlaybackStopped);
			}
		}
	}

	if (processed > 0 && processed != _samples) {
		warning("processed: %d, _samples: %d", processed, _samples);
	}
}
