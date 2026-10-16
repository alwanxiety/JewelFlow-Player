#include "SmtcManager.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <systemmediatransportcontrolsinterop.h>
#include <shobjidl.h>
#include <filesystem>
#include <iostream>

using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {

enum class MediaIconType {
    Prev,
    Play,
    Pause,
    Next
};

static HICON createMediaIcon(MediaIconType type, int size = 20) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdcMem = CreateCompatibleDC(nullptr);
    if (!hdcMem) return nullptr;

    HBITMAP hbmColor = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmColor || !bits) {
        DeleteDC(hdcMem);
        return nullptr;
    }

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmColor);
    memset(bits, 0, size * size * 4);

    HBRUSH whiteBrush = CreateSolidBrush(RGB(242, 245, 252));
    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdcMem, whiteBrush);
    SelectObject(hdcMem, nullPen);

    float s = static_cast<float>(size);
    if (type == MediaIconType::Play) {
        POINT pts[3] = {
            { static_cast<LONG>(s * 0.30f), static_cast<LONG>(s * 0.20f) },
            { static_cast<LONG>(s * 0.78f), static_cast<LONG>(s * 0.50f) },
            { static_cast<LONG>(s * 0.30f), static_cast<LONG>(s * 0.80f) }
        };
        Polygon(hdcMem, pts, 3);
    } else if (type == MediaIconType::Pause) {
        RECT r1 = { static_cast<LONG>(s * 0.28f), static_cast<LONG>(s * 0.20f),
                    static_cast<LONG>(s * 0.44f), static_cast<LONG>(s * 0.80f) };
        RECT r2 = { static_cast<LONG>(s * 0.56f), static_cast<LONG>(s * 0.20f),
                    static_cast<LONG>(s * 0.72f), static_cast<LONG>(s * 0.80f) };
        FillRect(hdcMem, &r1, whiteBrush);
        FillRect(hdcMem, &r2, whiteBrush);
    } else if (type == MediaIconType::Prev) {
        RECT bar = { static_cast<LONG>(s * 0.18f), static_cast<LONG>(s * 0.22f),
                     static_cast<LONG>(s * 0.28f), static_cast<LONG>(s * 0.78f) };
        FillRect(hdcMem, &bar, whiteBrush);
        POINT pts[3] = {
            { static_cast<LONG>(s * 0.76f), static_cast<LONG>(s * 0.22f) },
            { static_cast<LONG>(s * 0.32f), static_cast<LONG>(s * 0.50f) },
            { static_cast<LONG>(s * 0.76f), static_cast<LONG>(s * 0.78f) }
        };
        Polygon(hdcMem, pts, 3);
    } else if (type == MediaIconType::Next) {
        POINT pts[3] = {
            { static_cast<LONG>(s * 0.24f), static_cast<LONG>(s * 0.22f) },
            { static_cast<LONG>(s * 0.68f), static_cast<LONG>(s * 0.50f) },
            { static_cast<LONG>(s * 0.24f), static_cast<LONG>(s * 0.78f) }
        };
        Polygon(hdcMem, pts, 3);
        RECT bar = { static_cast<LONG>(s * 0.72f), static_cast<LONG>(s * 0.22f),
                     static_cast<LONG>(s * 0.82f), static_cast<LONG>(s * 0.78f) };
        FillRect(hdcMem, &bar, whiteBrush);
    }

    DeleteObject(whiteBrush);
    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);

    // Устанавливаем 32-битный Alpha-канал (ARGB) для непрозрачных белых пикселей
    uint32_t* pPixels = static_cast<uint32_t*>(bits);
    for (int i = 0; i < size * size; ++i) {
        uint32_t px = pPixels[i] & 0x00FFFFFF;
        if (px != 0) {
            pPixels[i] = 0xFF000000 | px;
        } else {
            pPixels[i] = 0;
        }
    }

    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;

    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);

    return hIcon;
}

constexpr UINT kBtnIdPrev = 1001;
constexpr UINT kBtnIdPlayPause = 1002;
constexpr UINT kBtnIdNext = 1003;

} // anonymous namespace

struct SmtcManager::Impl {
    HWND hwnd = nullptr;
    SystemMediaTransportControls smtc{ nullptr };
    winrt::event_token buttonToken{};

    ITaskbarList3* taskbarList = nullptr;
    UINT taskbarCreatedMsg = 0;
    bool thumbButtonsAdded = false;

    HICON iconPrev = nullptr;
    HICON iconPlay = nullptr;
    HICON iconPause = nullptr;
    HICON iconNext = nullptr;

    bool lastPlaying = false;
    std::string lastTitle;
    std::string lastArtist;
    std::string lastAlbum;
    std::wstring lastCover;

    void addThumbButtons() {
        if (!taskbarList || !hwnd || thumbButtonsAdded) return;

        THUMBBUTTON buttons[3] = {};

        buttons[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[0].iId = kBtnIdPrev;
        buttons[0].hIcon = iconPrev;
        buttons[0].dwFlags = THBF_ENABLED;
        wcscpy_s(buttons[0].szTip, L"Предыдущий трек");

        buttons[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[1].iId = kBtnIdPlayPause;
        buttons[1].hIcon = lastPlaying ? iconPause : iconPlay;
        buttons[1].dwFlags = THBF_ENABLED;
        wcscpy_s(buttons[1].szTip, lastPlaying ? L"Пауза" : L"Воспроизведение");

        buttons[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        buttons[2].iId = kBtnIdNext;
        buttons[2].hIcon = iconNext;
        buttons[2].dwFlags = THBF_ENABLED;
        wcscpy_s(buttons[2].szTip, L"Следующий трек");

        HRESULT hr = taskbarList->ThumbBarAddButtons(hwnd, 3, buttons);
        if (SUCCEEDED(hr)) {
            thumbButtonsAdded = true;
        }
    }
};

SmtcManager& SmtcManager::instance() {
    static SmtcManager s_instance;
    return s_instance;
}

SmtcManager::SmtcManager() : m_impl(new Impl()) {}

SmtcManager::~SmtcManager() {
    shutdown();
    delete m_impl;
}

bool SmtcManager::init(HWND hwnd) {
    if (!hwnd || !m_impl) return false;
    m_impl->hwnd = hwnd;
    m_impl->taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");

    m_impl->iconPrev = createMediaIcon(MediaIconType::Prev);
    m_impl->iconPlay = createMediaIcon(MediaIconType::Play);
    m_impl->iconPause = createMediaIcon(MediaIconType::Pause);
    m_impl->iconNext = createMediaIcon(MediaIconType::Next);

    // 1. Инициализация панели задач ITaskbarList3 для кнопок превью миниатюры
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&m_impl->taskbarList);
    if (SUCCEEDED(hr) && m_impl->taskbarList) {
        m_impl->taskbarList->HrInit();
        m_impl->addThumbButtons();
    }

    // 2. Инициализация SMTC (шторка быстрых настроек Win 10/11 и системные медиа-кнопки)
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        hr = interop->GetForWindow(hwnd, winrt::guid_of<ISystemMediaTransportControls>(), winrt::put_abi(m_impl->smtc));
        if (SUCCEEDED(hr) && m_impl->smtc) {
            m_impl->smtc.IsEnabled(true);
            m_impl->smtc.IsPlayEnabled(true);
            m_impl->smtc.IsPauseEnabled(true);
            m_impl->smtc.IsNextEnabled(true);
            m_impl->smtc.IsPreviousEnabled(true);
            m_impl->smtc.IsStopEnabled(true);
            m_impl->smtc.PlaybackStatus(MediaPlaybackStatus::Closed);

            m_impl->buttonToken = m_impl->smtc.ButtonPressed([this](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                auto btn = args.Button();
                if (btn == SystemMediaTransportControlsButton::Play) {
                    postAction(MediaAction::Play);
                } else if (btn == SystemMediaTransportControlsButton::Pause) {
                    postAction(MediaAction::Pause);
                } else if (btn == SystemMediaTransportControlsButton::Next) {
                    postAction(MediaAction::NextTrack);
                } else if (btn == SystemMediaTransportControlsButton::Previous) {
                    postAction(MediaAction::PrevTrack);
                } else if (btn == SystemMediaTransportControlsButton::Stop) {
                    postAction(MediaAction::Stop);
                }
            });
        }
    } catch (...) {}

    return true;
}

void SmtcManager::shutdown() {
    if (!m_impl) return;

    if (m_impl->smtc) {
        try {
            if (m_impl->buttonToken.value != 0) {
                m_impl->smtc.ButtonPressed(m_impl->buttonToken);
                m_impl->buttonToken = {};
            }
            m_impl->smtc.PlaybackStatus(MediaPlaybackStatus::Closed);
            m_impl->smtc.IsEnabled(false);
            m_impl->smtc = nullptr;
        } catch (...) {}
    }

    if (m_impl->taskbarList) {
        m_impl->taskbarList->Release();
        m_impl->taskbarList = nullptr;
    }

    if (m_impl->iconPrev) { DestroyIcon(m_impl->iconPrev); m_impl->iconPrev = nullptr; }
    if (m_impl->iconPlay) { DestroyIcon(m_impl->iconPlay); m_impl->iconPlay = nullptr; }
    if (m_impl->iconPause) { DestroyIcon(m_impl->iconPause); m_impl->iconPause = nullptr; }
    if (m_impl->iconNext) { DestroyIcon(m_impl->iconNext); m_impl->iconNext = nullptr; }
}

void SmtcManager::updateTrack(const std::string& title,
                             const std::string& artist,
                             const std::string& album,
                             const std::wstring& coverPath) {
    if (!m_impl) return;
    m_impl->lastTitle = title;
    m_impl->lastArtist = artist;
    m_impl->lastAlbum = album;
    m_impl->lastCover = coverPath;

    if (!m_impl->smtc) return;

    try {
        auto updater = m_impl->smtc.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        auto music = updater.MusicProperties();

        music.Title(winrt::to_hstring(title.empty() ? "JewelPlayer" : title));
        music.Artist(winrt::to_hstring(artist.empty() ? "Неизвестный исполнитель" : artist));
        if (!album.empty()) {
            music.AlbumTitle(winrt::to_hstring(album));
        }

        if (!coverPath.empty() && std::filesystem::exists(coverPath)) {
            try {
                auto file = StorageFile::GetFileFromPathAsync(coverPath).get();
                updater.Thumbnail(RandomAccessStreamReference::CreateFromFile(file));
            } catch (...) {
                updater.Thumbnail(nullptr);
            }
        } else {
            updater.Thumbnail(nullptr);
        }

        updater.Update();
    } catch (...) {}
}

void SmtcManager::updatePlaybackState(bool isPlaying) {
    if (!m_impl) return;
    m_impl->lastPlaying = isPlaying;

    if (m_impl->smtc) {
        try {
            m_impl->smtc.PlaybackStatus(isPlaying ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
        } catch (...) {}
    }

    if (m_impl->taskbarList && m_impl->hwnd) {
        if (!m_impl->thumbButtonsAdded) {
            m_impl->addThumbButtons();
        } else {
            THUMBBUTTON buttons[3] = {};

            buttons[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            buttons[0].iId = kBtnIdPrev;
            buttons[0].hIcon = m_impl->iconPrev;
            buttons[0].dwFlags = THBF_ENABLED;
            wcscpy_s(buttons[0].szTip, L"Предыдущий трек");

            buttons[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            buttons[1].iId = kBtnIdPlayPause;
            buttons[1].hIcon = isPlaying ? m_impl->iconPause : m_impl->iconPlay;
            buttons[1].dwFlags = THBF_ENABLED;
            wcscpy_s(buttons[1].szTip, isPlaying ? L"Пауза" : L"Воспроизведение");

            buttons[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            buttons[2].iId = kBtnIdNext;
            buttons[2].hIcon = m_impl->iconNext;
            buttons[2].dwFlags = THBF_ENABLED;
            wcscpy_s(buttons[2].szTip, L"Следующий трек");

            m_impl->taskbarList->ThumbBarUpdateButtons(m_impl->hwnd, 3, buttons);
        }
    }
}

void SmtcManager::postAction(MediaAction action) {
    {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actionQueue.push_back(action);
    }
    if (m_impl && m_impl->hwnd) {
        PostMessageW(m_impl->hwnd, WM_NULL, 0, 0);
    }
}

MediaAction SmtcManager::pollAction() {
    std::lock_guard<std::mutex> lock(m_actionMutex);
    if (m_actionQueue.empty()) return MediaAction::None;
    MediaAction act = m_actionQueue.front();
    m_actionQueue.erase(m_actionQueue.begin());
    return act;
}

LRESULT SmtcManager::handleWindowMessage(HWND h, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled) {
    if (!m_impl) return 0;

    if (msg == m_impl->taskbarCreatedMsg && m_impl->taskbarCreatedMsg != 0) {
        m_impl->thumbButtonsAdded = false;
        m_impl->addThumbButtons();
        handled = true;
        return 0;
    }

    if (msg == WM_COMMAND && HIWORD(wParam) == 0) {
        UINT id = LOWORD(wParam);
        if (id == kBtnIdPrev) {
            postAction(MediaAction::PrevTrack);
            handled = true;
            return 0;
        } else if (id == kBtnIdPlayPause) {
            postAction(MediaAction::TogglePlayPause);
            handled = true;
            return 0;
        } else if (id == kBtnIdNext) {
            postAction(MediaAction::NextTrack);
            handled = true;
            return 0;
        }
    }

    if (msg == WM_APPCOMMAND) {
        int cmd = GET_APPCOMMAND_LPARAM(lParam);
        if (cmd == APPCOMMAND_MEDIA_PLAY_PAUSE) {
            postAction(MediaAction::TogglePlayPause);
            handled = true;
            return TRUE;
        } else if (cmd == APPCOMMAND_MEDIA_PLAY) {
            postAction(MediaAction::Play);
            handled = true;
            return TRUE;
        } else if (cmd == APPCOMMAND_MEDIA_PAUSE) {
            postAction(MediaAction::Pause);
            handled = true;
            return TRUE;
        } else if (cmd == APPCOMMAND_MEDIA_NEXTTRACK) {
            postAction(MediaAction::NextTrack);
            handled = true;
            return TRUE;
        } else if (cmd == APPCOMMAND_MEDIA_PREVIOUSTRACK) {
            postAction(MediaAction::PrevTrack);
            handled = true;
            return TRUE;
        } else if (cmd == APPCOMMAND_MEDIA_STOP) {
            postAction(MediaAction::Stop);
            handled = true;
            return TRUE;
        }
    }

    return 0;
}
