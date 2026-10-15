#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "AudioEngine.hpp"
#include <algorithm>

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
    if (m_engine) {
        ma_engine_uninit(m_engine);
        delete m_engine;
        m_engine = nullptr;
    }
}

bool AudioEngine::init() {
    if (m_engine) return true;
    m_engine = new ma_engine();
    ma_engine_config config = ma_engine_config_init();
    // Увеличиваем период аудио-буфера до 20 мс для снижения числа прерываний и нагрузки на CPU
    config.periodSizeInMilliseconds = 20;
    if (ma_engine_init(&config, m_engine) != MA_SUCCESS) {
        delete m_engine;
        m_engine = nullptr;
        return false;
    }
    return true;
}

bool AudioEngine::playFile(const std::wstring& widePath, bool startPaused) {
    stop();

    if (!m_engine) return false;

    m_sound = new ma_sound();
    // Потоковое чтение + отключение 3D-спатиализации и питча для минимальной нагрузки на CPU
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
    ma_result result = ma_sound_init_from_file_w(m_engine, widePath.c_str(), flags, nullptr, nullptr, m_sound);
    if (result != MA_SUCCESS) {
        delete m_sound;
        m_sound = nullptr;
        return false;
    }

    m_currentPath = widePath;
    m_duration = 0.0f;
    m_lengthFrames = 0;
    ma_sound_get_length_in_seconds(m_sound, &m_duration);

    ma_uint64 frames = 0;
    if (ma_sound_get_length_in_pcm_frames(m_sound, &frames) == MA_SUCCESS) {
        m_lengthFrames = static_cast<uint64_t>(frames);
    }

    ma_sound_set_volume(m_sound, m_isMuted ? 0.0f : m_volume);
    m_isLoaded = true;

    if (startPaused) {
        m_state = PlaybackState::Paused;
    } else {
        ma_sound_start(m_sound);
        m_state = PlaybackState::Playing;
    }
    return true;
}

void AudioEngine::pause() {
    if (m_sound && m_state == PlaybackState::Playing) {
        ma_sound_stop(m_sound);
        m_state = PlaybackState::Paused;
    }
}

void AudioEngine::resume() {
    if (!m_sound || !m_isLoaded) return;

    if (m_state == PlaybackState::Paused) {
        if (isEnded()) {
            ma_sound_seek_to_pcm_frame(m_sound, 0);
        }
        ma_sound_start(m_sound);
        m_state = PlaybackState::Playing;
    } else if (m_state == PlaybackState::Stopped && !m_currentPath.empty()) {
        playFile(m_currentPath, false);
    }
}

void AudioEngine::togglePlayPause() {
    if (m_state == PlaybackState::Playing) {
        pause();
    } else if (m_state == PlaybackState::Paused || m_state == PlaybackState::Stopped) {
        resume();
    }
}

void AudioEngine::stop() {
    if (m_sound) {
        ma_sound_stop(m_sound);
        ma_sound_uninit(m_sound);
        delete m_sound;
        m_sound = nullptr;
    }
    m_state = PlaybackState::Stopped;
    m_duration = 0.0f;
    m_lengthFrames = 0;
    m_isLoaded = false;
}

void AudioEngine::seekTo(float normalizedTime) {
    if (!m_sound || !m_isLoaded || m_lengthFrames == 0) return;

    normalizedTime = std::clamp(normalizedTime, 0.0f, 1.0f);
    ma_uint64 targetFrame = static_cast<ma_uint64>(normalizedTime * static_cast<double>(m_lengthFrames));
    if (targetFrame >= m_lengthFrames && m_lengthFrames > 0) {
        targetFrame = static_cast<ma_uint64>(m_lengthFrames - 1);
    }
    ma_sound_seek_to_pcm_frame(m_sound, targetFrame);
}

void AudioEngine::seekBySeconds(float deltaSeconds) {
    if (!m_sound || !m_isLoaded || m_duration <= 0.0f) return;
    float cur = getPlaybackPosition();
    float target = std::clamp(cur + deltaSeconds, 0.0f, m_duration);
    seekTo(target / m_duration);
}

void AudioEngine::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_volume > 0.0f && m_isMuted) {
        m_isMuted = false;
    }
    if (m_sound && m_isLoaded) {
        ma_sound_set_volume(m_sound, m_isMuted ? 0.0f : m_volume);
    }
}

void AudioEngine::toggleMute() {
    m_isMuted = !m_isMuted;
    if (m_sound && m_isLoaded) {
        ma_sound_set_volume(m_sound, m_isMuted ? 0.0f : m_volume);
    }
}

bool AudioEngine::isEnded() const {
    if (!m_sound || !m_isLoaded || m_state != PlaybackState::Playing) return false;
    return ma_sound_at_end(m_sound) == MA_TRUE;
}

float AudioEngine::getPlaybackPosition() const {
    if (!m_sound || !m_isLoaded) return 0.0f;
    float cursor = 0.0f;
    ma_sound_get_cursor_in_seconds(m_sound, &cursor);
    return cursor;
}
