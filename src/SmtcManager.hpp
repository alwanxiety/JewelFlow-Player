#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

enum class MediaAction {
    None,
    TogglePlayPause,
    Play,
    Pause,
    NextTrack,
    PrevTrack,
    Stop
};

class SmtcManager {
public:
    static SmtcManager& instance();

    bool init(HWND hwnd);
    void shutdown();

    void updateTrack(const std::string& title,
                     const std::string& artist,
                     const std::string& album,
                     const std::wstring& coverPath);

    void updatePlaybackState(bool isPlaying);

    void postAction(MediaAction action);
    MediaAction pollAction();

    LRESULT handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled);

private:
    SmtcManager();
    ~SmtcManager();

    struct Impl;
    Impl* m_impl = nullptr;
    std::mutex m_actionMutex;
    std::vector<MediaAction> m_actionQueue;
};
