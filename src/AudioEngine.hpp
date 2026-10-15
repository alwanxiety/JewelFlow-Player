#pragma once
#include <string>
#include <cstdint>

struct ma_engine;
struct ma_sound;

enum class PlaybackState {
    Stopped,
    Playing,
    Paused
};

enum class LoopMode {
    None,
    Track,
    All
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    bool playFile(const std::wstring& widePath, bool startPaused = false);
    void pause();
    void resume();
    void togglePlayPause();
    void stop();

    void seekTo(float normalizedTime);
    void seekBySeconds(float deltaSeconds);
    void setVolume(float volume);
    void toggleMute();

    PlaybackState getState() const { return m_state; }
    bool isPlaying() const { return m_state == PlaybackState::Playing; }
    bool isLoaded() const { return m_isLoaded; }
    bool isEnded() const;
    bool isMuted() const { return m_isMuted; }

    float getPlaybackPosition() const;
    float getDuration() const { return m_duration; }
    float getVolume() const { return m_volume; }

    void setLoopMode(LoopMode mode) { m_loopMode = mode; }
    LoopMode getLoopMode() const { return m_loopMode; }

private:
    ma_engine* m_engine = nullptr;
    ma_sound* m_sound = nullptr;
    std::wstring m_currentPath;
    PlaybackState m_state = PlaybackState::Stopped;
    LoopMode m_loopMode = LoopMode::All;
    float m_volume = 0.85f;
    float m_duration = 0.0f;
    uint64_t m_lengthFrames = 0;
    bool m_isMuted = false;
    bool m_isLoaded = false;
};