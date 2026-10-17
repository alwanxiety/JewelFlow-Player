#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <psapi.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <filesystem>
#include <fstream>
#include <vector>
#include <deque>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <random>
#include <iomanip>
#include <sstream>
#include <cmath>

#include "AudioEngine.hpp"
#include "Metadata.hpp"
#include "LyricsFetcher.hpp"
#include "CoverFetcher.hpp"
#include "Renderer.hpp"
#include "SmtcManager.hpp"
#include "YandexMusicManager.hpp"

namespace fs = std::filesystem;

static bool g_focusSearchInput = false;
static bool g_showHotkeysDialog = false;
static bool g_showYmAuthModal = false;
static char g_ymTokenInput[1024] = {0};
static std::string g_ymLastSearchQuery;
static double g_ymLastSearchTypingTime = 0.0;
static constexpr float kSpacing = 0.21f;

static std::vector<TrackData> g_playlist;
static std::vector<float> g_hoverWeights;
static std::vector<int> g_visibleIndices;
static std::deque<std::wstring> g_pendingLoadQueue;
static int g_pendingRestoreTrack = -1;
static bool g_searchDirty = true;

static int g_selectedTrack = -1;
static int g_activePlayingTrack = -1;
static float g_shelfScroll = 0.0f;
static float g_targetScroll = 0.0f;
static bool g_isShuffle = false;
static char g_searchQuery[128] = {0};
static std::string g_lastSearchQuery;

enum class SearchSource {
    Local,
    Yandex
};
static SearchSource g_searchSource = SearchSource::Local;
static SearchSource g_lastSearchSource = SearchSource::Local;
static std::unordered_map<std::string, GLuint> s_ymThumbTextures;

// Состояние 3D-разворота бокса задней стороной (режим Lyrics / Караоке)
static bool g_lyricsInspectMode = false;
static bool g_inspectShowFront = false;
static float g_inspectProgress = 0.0f;
static float g_inspectFrontFlip = 0.0f;
static float g_karaokeScrollY = 0.0f;
static int g_lastLyricsTrackIdx = -1;
static double g_lastScrollTime = 0.0;
static AAMode g_savedAAMode = AAMode::MSAA;
static bool g_savedReflections = true;
static bool g_boldInterfaceFont = false;
static int g_monitorRefreshRate = 60;

// ------------------ ДЕБАГ-МОНИТОР ПРОИЗВОДИТЕЛЬНОСТИ (F3) ------------------
static bool g_showDebugOverlay = false;
static bool g_debugOverlayFirstOpen = true;

// 1. Аппаратный замер GPU через кольцевой буфер GL_TIME_ELAPSED (0% CPU оверхед, совпадает с Диспетчером задач 3D)
static constexpr int kGpuQueryCount = 4;
static GLuint g_gpuQueries[kGpuQueryCount] = {0};
static bool g_gpuQueryInFlight[kGpuQueryCount] = {false};
static int g_gpuQueryWriteIdx = 0;
static bool g_gpuQueriesInitialized = false;
static uint64_t g_accumGpuTimeNs = 0;

// 2. Системный замер ЦПУ и Памяти через GetProcessTimes и GetProcessMemoryInfo (совпадает с Диспетчером задач)
static uint64_t g_prevProcTime = 0;
static uint64_t g_prevWallTime = 0;
static DWORD g_numLogicalCores = 1;
static double g_lastTelemetrySampleTime = 0.0;
static int g_telemetryFrameCounter = 0;

// Отображаемые сглаженные метрики
static float g_telemetryFps = 0.0f;
static float g_telemetryFrameMs = 0.0f;
static float g_telemetryCpuPercent = 0.0f;
static float g_telemetryGpuPercent = 0.0f;
static float g_telemetryRamMb = 0.0f;

static inline uint64_t fileTimeToUint64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static ImFont* g_fontMain = nullptr;
static ImFont* g_fontMainBold = nullptr;
static ImFont* g_fontTitleBold = nullptr;
static ImFont* g_fontArtistMedium = nullptr;
static ImFont* g_fontLyricsRegular = nullptr;
static ImFont* g_fontKaraokeBold = nullptr;

// Респонсивные шрифты для Fullscreen 1080p (Medium) и Fullscreen 1440p / 4K (Large)
static ImFont* g_fontMainMed = nullptr;
static ImFont* g_fontTitleMed = nullptr;
static ImFont* g_fontArtistMed = nullptr;
static ImFont* g_fontLyricsMed = nullptr;
static ImFont* g_fontKaraokeMed = nullptr;

static ImFont* g_fontMainLarge = nullptr;
static ImFont* g_fontTitleLarge = nullptr;
static ImFont* g_fontArtistLarge = nullptr;
static ImFont* g_fontLyricsLarge = nullptr;
static ImFont* g_fontKaraokeLarge = nullptr;
static LyricsFetcher g_lyricsFetcher;
static CoverFetcher g_coverFetcher;

struct RouletteState {
    bool active = false;
    int targetSlot = 0;
    int winSlotStart = 0;
    int winSlotEnd = 0;
    int currentSlot = -1;
    int currentTrack = -1;
    int stepsRemaining = 0;
    int totalSteps = 0;
    float stepTimer = 0.0f;
    float stepDuration = 0.06f;
} static g_roulette;

static int g_contextMenuTrack = -1;
static bool g_showSettingsWindow = false;
static float g_settingsAnimProgress = 0.0f;
static float g_settingsGearRotation = 0.0f;
static WNDPROC g_origWndProc = nullptr;

static std::mt19937& getRng() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

static LRESULT CALLBACK CustomBorderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    bool handled = false;
    LRESULT lr = SmtcManager::instance().handleWindowMessage(hwnd, msg, wParam, lParam, handled);
    if (handled) return lr;

    if (msg == WM_NCCALCSIZE && wParam == TRUE) {
        NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
        if (IsZoomed(hwnd)) {
            int frameX = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            int frameY = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            params->rgrc[0].left   += frameX;
            params->rgrc[0].top    += frameY;
            params->rgrc[0].right  -= frameX;
            params->rgrc[0].bottom -= frameY;
        }
        return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void applyWindows11FrameStyle(HWND hwnd) {
    if (!hwnd) return;

    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CustomBorderlessWndProc))
    );

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= (WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

    COLORREF borderCol = RGB(36, 40, 52);
    DwmSetWindowAttribute(hwnd, 34, &borderCol, sizeof(borderCol));

    BOOL transitionsDisabled = FALSE;
    DwmSetWindowAttribute(hwnd, 3, &transitionsDisabled, sizeof(transitionsDisabled));

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static fs::path getExeDir() {
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        return fs::path(exePath).parent_path();
    }
    return fs::current_path();
}

static fs::path getSavedPlaylistPath() {
    return getExeDir() / L"playlist.txt";
}

static fs::path getSavedSettingsPath() {
    return getExeDir() / L"settings.cfg";
}

static void savePlaylistToDisk() {
    std::ofstream out(getSavedPlaylistPath(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    for (const auto& t : g_playlist) {
        std::string utf8Line = MetadataParser::wideToUtf8(t.widePath);
        if (!utf8Line.empty()) {
            out << utf8Line << "\n";
        }
    }
    for (const auto& wp : g_pendingLoadQueue) {
        std::string utf8Line = MetadataParser::wideToUtf8(wp);
        if (!utf8Line.empty()) {
            out << utf8Line << "\n";
        }
    }
}

static void saveSettingsToDisk(const AudioEngine& audio) {
    std::ofstream out(getSavedSettingsPath(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    out << "volume=" << audio.getVolume() << "\n";
    out << "shuffle=" << (g_isShuffle ? 1 : 0) << "\n";
    out << "loop=" << static_cast<int>(audio.getLoopMode()) << "\n";
    out << "lastTrack=" << g_activePlayingTrack << "\n";
    out << "aa=" << static_cast<int>(g_savedAAMode) << "\n";
    out << "debug=" << (g_showDebugOverlay ? 1 : 0) << "\n";
    out << "reflections=" << (g_savedReflections ? 1 : 0) << "\n";
    out << "boldFont=" << (g_boldInterfaceFont ? 1 : 0) << "\n";
}

static void loadSettingsFromDisk(AudioEngine& audio) {
    std::ifstream in(getSavedSettingsPath(), std::ios::binary);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        try {
            if (key == "volume") audio.setVolume(std::stof(val));
            else if (key == "shuffle") g_isShuffle = (std::stoi(val) != 0);
            else if (key == "loop") {
                int lm = std::clamp(std::stoi(val), 0, 2);
                audio.setLoopMode(static_cast<LoopMode>(lm));
            } else if (key == "lastTrack") {
                g_pendingRestoreTrack = std::stoi(val);
            } else if (key == "aa") {
                int aa = std::clamp(std::stoi(val), 0, 2);
                g_savedAAMode = static_cast<AAMode>(aa);
            } else if (key == "debug") {
                g_showDebugOverlay = (std::stoi(val) != 0);
            } else if (key == "reflections") {
                g_savedReflections = (std::stoi(val) != 0);
            } else if (key == "boldFont") {
                g_boldInterfaceFont = (std::stoi(val) != 0);
            }
        } catch (...) {}
    }
}

// Запускает фоновый автопоиск текста для трека (LRCLIB Synced -> LRCLIB Plain -> Genius)
static void triggerAutoLyricsForTrack(int trackIdx, const AudioEngine& audio) {
    if (trackIdx < 0 || trackIdx >= static_cast<int>(g_playlist.size())) return;
    const auto& tr = g_playlist[trackIdx];
    int durSec = (trackIdx == g_activePlayingTrack) ? static_cast<int>(std::round(audio.getDuration())) : 0;
    g_lyricsFetcher.requestLyrics(tr.widePath, tr.title, tr.artist, durSec, tr.embeddedLyrics);
    if (g_lastLyricsTrackIdx != trackIdx) {
        g_lastLyricsTrackIdx = trackIdx;
        g_karaokeScrollY = 0.0f;
    }
}

static void playTrackAndFetchLyrics(int trackIdx, AudioEngine& audio, float& discFlightProgress, bool startPaused = false) {
    if (trackIdx < 0 || trackIdx >= static_cast<int>(g_playlist.size())) return;
    g_activePlayingTrack = trackIdx;
    g_selectedTrack = trackIdx;
    discFlightProgress = startPaused ? 1.0f : 0.0f;
    audio.playFile(g_playlist[trackIdx].widePath, startPaused);
    triggerAutoLyricsForTrack(trackIdx, audio);

    const auto& tr = g_playlist[trackIdx];
    SmtcManager::instance().updateTrack(tr.title, tr.artist, tr.album, tr.coverImagePath);
    SmtcManager::instance().updatePlaybackState(!startPaused);
}

static bool isSupportedAudioExt(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return (ext == L".mp3" || ext == L".flac" || ext == L".wav");
}

static bool isTrackAlreadyKnown(const std::wstring& path) {
    for (const auto& t : g_playlist) {
        if (t.widePath == path) return true;
    }
    for (const auto& q : g_pendingLoadQueue) {
        if (q == path) return true;
    }
    return false;
}

static bool addTrackImmediate(const std::wstring& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    for (const auto& t : g_playlist) {
        if (t.widePath == path) return false;
    }
    g_playlist.push_back(MetadataParser::parse(path, static_cast<int>(g_playlist.size())));
    if (!g_playlist.back().hasRealCover) {
        g_coverFetcher.requestCover(g_playlist.back().widePath, g_playlist.back().artist, g_playlist.back().title, g_playlist.back().album);
    }
    g_hoverWeights.push_back(0.0f);
    g_searchDirty = true;
    return true;
}

static bool enqueueTrackForLoad(const std::wstring& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || isTrackAlreadyKnown(path)) return false;
    g_pendingLoadQueue.push_back(path);
    return true;
}

static void addPathOrDirectory(const fs::path& p) {
    std::error_code ec;
    bool addedAny = false;
    if (fs::is_directory(p, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec) && isSupportedAudioExt(entry.path())) {
                if (enqueueTrackForLoad(entry.path().wstring())) {
                    addedAny = true;
                }
            }
        }
    } else if (fs::is_regular_file(p, ec) && isSupportedAudioExt(p)) {
        if (enqueueTrackForLoad(p.wstring())) {
            addedAny = true;
        }
    }
    if (addedAny) {
        savePlaylistToDisk();
    }
}

static void queuePlaylistFromDisk() {
    std::ifstream in(getSavedPlaylistPath(), std::ios::binary);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        std::wstring wp = MetadataParser::utf8ToWide(line);
        if (!wp.empty()) {
            enqueueTrackForLoad(wp);
        }
    }
}

static void rebuildVisibleIndicesIfNeeded() {
    if (!g_searchDirty && g_lastSearchQuery == g_searchQuery && g_lastSearchSource == g_searchSource) return;
    g_lastSearchQuery = g_searchQuery;
    g_lastSearchSource = g_searchSource;
    g_searchDirty = false;

    g_visibleIndices.clear();
    if (g_searchSource == SearchSource::Yandex) {
        // В режиме онлайн-поиска Яндекс Музыки не скрываем локальную полку
        for (size_t i = 0; i < g_playlist.size(); ++i) {
            g_visibleIndices.push_back(static_cast<int>(i));
        }
    } else {
        std::wstring wQuery = MetadataParser::toLowerWide(MetadataParser::utf8ToWide(g_searchQuery));
        for (size_t i = 0; i < g_playlist.size(); ++i) {
            if (wQuery.empty() || g_playlist[i].searchKeyLower.find(wQuery) != std::wstring::npos) {
                g_visibleIndices.push_back(static_cast<int>(i));
            }
        }
    }
}

static void openFileDialog(HWND hwnd) {
    wchar_t buf[32768] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Audio Files (*.mp3;*.flac;*.wav)\0*.mp3;*.flac;*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf) / sizeof(wchar_t);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        bool addedAny = false;
        wchar_t* ptr = ofn.lpstrFile;
        std::wstring dir = ptr;
        ptr += dir.length() + 1;
        if (*ptr == L'\0') {
            addedAny = enqueueTrackForLoad(dir);
        } else {
            while (*ptr != L'\0') {
                if (enqueueTrackForLoad(dir + L"\\" + ptr)) {
                    addedAny = true;
                }
                ptr += wcslen(ptr) + 1;
            }
        }
        if (addedAny) {
            savePlaylistToDisk();
        }
    }
}

static void openFolderDialog(HWND hwnd) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Выберите папку с музыкой (MP3 / FLAC / WAV):";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path)) {
            addPathOrDirectory(fs::path(path));
        }
        CoTaskMemFree(pidl);
    }
}

static void dropCallback(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) {
        std::wstring wp = MetadataParser::utf8ToWide(paths[i]);
        if (!wp.empty()) {
            addPathOrDirectory(fs::path(wp));
        }
    }
}

static void scrollCallback(GLFWwindow*, double xoffset, double yoffset) {
    if (!ImGui::GetIO().WantCaptureMouse && !g_roulette.active && !g_lyricsInspectMode) {
        float delta = (std::abs(xoffset) > std::abs(yoffset)) ? -static_cast<float>(xoffset) : static_cast<float>(yoffset);
        // Плавный и информативный скролл: 1 щелчок колеса = 2 трека (0.42f)
        g_targetScroll -= delta * (kSpacing * 2.0f);
        if (!g_visibleIndices.empty()) {
            float maxScroll = (std::max)(0.0f, (static_cast<float>(g_visibleIndices.size()) - 1.0f) * kSpacing);
            const float overscroll = kSpacing * 1.5f;
            g_targetScroll = std::clamp(g_targetScroll, -overscroll, maxScroll + overscroll);
        }
        g_lastScrollTime = glfwGetTime();
    }
}

static std::string formatTime(float seconds) {
    if (seconds < 0.0f || std::isnan(seconds)) seconds = 0.0f;
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    std::ostringstream ss;
    ss << m << ":" << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

static std::string truncateUtf8(const std::string& str, size_t maxCodepoints) {
    size_t cpCount = 0;
    size_t byteIdx = 0;
    while (byteIdx < str.size()) {
        if (cpCount == maxCodepoints) {
            return str.substr(0, byteIdx) + "...";
        }
        unsigned char c = static_cast<unsigned char>(str[byteIdx]);
        size_t charLen = 1;
        if ((c & 0x80) == 0x00) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;

        if (byteIdx + charLen > str.size()) break;
        byteIdx += charLen;
        cpCount++;
    }
    return str;
}

static void focusTrackOnShelf(int trackIdx, const std::vector<int>& visibleIndices) {
    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        if (visibleIndices[slot] == trackIdx) {
            float desired = static_cast<float>(slot) * kSpacing - 0.55f;
            float maxScroll = (std::max)(0.0f, (static_cast<float>(visibleIndices.size()) - 1.0f) * kSpacing);
            g_targetScroll = std::clamp(desired, -0.35f, maxScroll + 0.35f);
            break;
        }
    }
}

static void startRandomRoulette(const std::vector<int>& visibleIndices, AudioEngine& audio, float& discFlightProgress) {
    if (visibleIndices.empty()) return;
    g_lyricsInspectMode = false;
    int count = static_cast<int>(visibleIndices.size());
    if (count == 1) {
        int onlyIdx = visibleIndices[0];
        playTrackAndFetchLyrics(onlyIdx, audio, discFlightProgress);
        focusTrackOnShelf(onlyIdx, visibleIndices);
        return;
    }

    std::uniform_int_distribution<int> slotDist(0, count - 1);
    int targetSlot = slotDist(getRng());
    if (visibleIndices[targetSlot] == g_activePlayingTrack && count > 1) {
        targetSlot = (targetSlot + 1) % count;
    }

    int halfWindow = 5;
    int winStart = std::max(0, targetSlot - halfWindow);
    int winEnd = std::min(count - 1, winStart + 10);
    winStart = std::max(0, winEnd - 10);

    int centerSlot = (winStart + winEnd) / 2;
    focusTrackOnShelf(visibleIndices[centerSlot], visibleIndices);

    std::uniform_int_distribution<int> stepDist(13, 17);
    g_roulette.active = true;
    g_roulette.targetSlot = targetSlot;
    g_roulette.winSlotStart = winStart;
    g_roulette.winSlotEnd = winEnd;
    g_roulette.totalSteps = stepDist(getRng());
    g_roulette.stepsRemaining = g_roulette.totalSteps;
    g_roulette.stepTimer = 0.0f;
    g_roulette.stepDuration = 0.06f;

    g_roulette.currentSlot = winStart + (targetSlot + 1 - winStart) % std::max(1, winEnd - winStart + 1);
    g_roulette.currentTrack = visibleIndices[g_roulette.currentSlot];
}

static int getNextTrackIndex(const std::vector<int>& activeList, bool allowWrap) {
    if (activeList.empty()) return -1;
    if (g_isShuffle && activeList.size() > 1) {
        std::uniform_int_distribution<size_t> dis(0, activeList.size() - 1);
        int next = g_activePlayingTrack;
        for (int tries = 0; tries < 8 && next == g_activePlayingTrack; ++tries) {
            next = activeList[dis(getRng())];
        }
        return next;
    }
    for (size_t i = 0; i < activeList.size(); ++i) {
        if (activeList[i] == g_activePlayingTrack) {
            if (i + 1 < activeList.size()) return activeList[i + 1];
            return allowWrap ? activeList[0] : -1;
        }
    }
    return activeList[0];
}

static int getPrevTrackIndex(const std::vector<int>& activeList) {
    if (activeList.empty()) return -1;
    for (size_t i = 0; i < activeList.size(); ++i) {
        if (activeList[i] == g_activePlayingTrack) {
            if (i > 0) return activeList[i - 1];
            return activeList.back();
        }
    }
    return activeList[0];
}

static void processMediaActions(AudioEngine& audio, float& discFlightProgress, double now, double& lastActiveTime) {
    while (true) {
        MediaAction smtcAction = SmtcManager::instance().pollAction();
        if (smtcAction == MediaAction::None) break;

        std::vector<int> fallbackIndices;
        const std::vector<int>* listToUse = &g_visibleIndices;
        if (listToUse->empty() && !g_playlist.empty()) {
            fallbackIndices.resize(g_playlist.size());
            for (size_t i = 0; i < g_playlist.size(); ++i) fallbackIndices[i] = static_cast<int>(i);
            listToUse = &fallbackIndices;
        }

        if (smtcAction == MediaAction::TogglePlayPause) {
            if (g_activePlayingTrack != -1) {
                audio.togglePlayPause();
            } else if (!listToUse->empty()) {
                playTrackAndFetchLyrics((*listToUse)[0], audio, discFlightProgress);
            }
        } else if (smtcAction == MediaAction::Play) {
            if (g_activePlayingTrack != -1) {
                if (!audio.isPlaying()) audio.togglePlayPause();
            } else if (!listToUse->empty()) {
                playTrackAndFetchLyrics((*listToUse)[0], audio, discFlightProgress);
            }
        } else if (smtcAction == MediaAction::Pause) {
            if (audio.isPlaying()) audio.togglePlayPause();
        } else if (smtcAction == MediaAction::NextTrack) {
            int nextIdx = getNextTrackIndex(*listToUse, true);
            if (nextIdx >= 0) {
                playTrackAndFetchLyrics(nextIdx, audio, discFlightProgress);
                focusTrackOnShelf(nextIdx, g_visibleIndices);
            }
        } else if (smtcAction == MediaAction::PrevTrack) {
            if (audio.getPlaybackPosition() > 3.0f) {
                audio.seekTo(0.0f);
            } else {
                int prevIdx = getPrevTrackIndex(*listToUse);
                if (prevIdx >= 0) {
                    playTrackAndFetchLyrics(prevIdx, audio, discFlightProgress);
                    focusTrackOnShelf(prevIdx, g_visibleIndices);
                }
            }
        } else if (smtcAction == MediaAction::Stop) {
            audio.stop();
        }
        lastActiveTime = now;
    }
}

static void checkTrackEndAndAdvance(AudioEngine& audio, float& discFlightProgress, double now, double& lastActiveTime) {
    if (audio.isEnded() && g_activePlayingTrack != -1) {
        LoopMode lm = audio.getLoopMode();
        if (lm == LoopMode::Track) {
            playTrackAndFetchLyrics(g_activePlayingTrack, audio, discFlightProgress);
        } else {
            bool allowWrap = (lm == LoopMode::All);
            std::vector<int> fallbackIndices;
            const std::vector<int>* listToUse = &g_visibleIndices;
            if (listToUse->empty() && !g_playlist.empty()) {
                fallbackIndices.resize(g_playlist.size());
                for (size_t i = 0; i < g_playlist.size(); ++i) fallbackIndices[i] = static_cast<int>(i);
                listToUse = &fallbackIndices;
            }
            int nextIdx = getNextTrackIndex(*listToUse, allowWrap);
            if (nextIdx >= 0 && nextIdx < static_cast<int>(g_playlist.size())) {
                playTrackAndFetchLyrics(nextIdx, audio, discFlightProgress);
                focusTrackOnShelf(nextIdx, g_visibleIndices);
            } else {
                audio.stop();
            }
        }
        lastActiveTime = now;
    }
}

static void syncSmtcPlaybackState(AudioEngine& audio) {
    static bool s_lastAudioPlaying = false;
    bool curAudioPlaying = audio.isPlaying();
    if (curAudioPlaying != s_lastAudioPlaying) {
        s_lastAudioPlaying = curAudioPlaying;
        SmtcManager::instance().updatePlaybackState(curAudioPlaying);
    }
}

static bool CustomScrubber(const char* label, float* value, float max_val, float width, float height, const ImVec4& accentColor, bool seekOnRelease = false, bool* outIsDragging = nullptr) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);

    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size(width, height);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, id, &hovered, &held);

    if (outIsDragging) {
        *outIsDragging = held;
    }

    float norm = (max_val > 0.0f) ? std::clamp(*value / max_val, 0.0f, 1.0f) : 0.0f;
    if (held && max_val > 0.0f) {
        float mouseX = g.IO.MousePos.x;
        norm = std::clamp((mouseX - bb.Min.x) / bb.GetWidth(), 0.0f, 1.0f);
        *value = norm * max_val;
    }

    float trackH = (hovered || held) ? 5.0f : 4.0f;
    float trackY = bb.Min.y + (bb.GetHeight() - trackH) * 0.5f;
    ImDrawList* drawList = window->DrawList;

    drawList->AddRectFilled(
        ImVec2(bb.Min.x, trackY),
        ImVec2(bb.Max.x, trackY + trackH),
        ImColor(255, 255, 255, 45),
        trackH * 0.5f
    );

    float fillW = bb.GetWidth() * norm;
    if (fillW > 1.0f) {
        drawList->AddRectFilled(
            ImVec2(bb.Min.x, trackY),
            ImVec2(bb.Min.x + fillW, trackY + trackH),
            ImColor(accentColor),
            trackH * 0.5f
        );
    }

    float thumbR = (hovered || held) ? 6.0f : 4.5f;
    drawList->AddCircleFilled(
        ImVec2(bb.Min.x + fillW, trackY + trackH * 0.5f),
        thumbR,
        ImColor(255, 255, 255, static_cast<int>(accentColor.w * 255.0f))
    );

    if (seekOnRelease) {
        return ImGui::IsItemDeactivated();
    }
    return held;
}

enum class IconType { Play, Pause, Prev, Next, Shuffle, Repeat, Volume, Mute, Dice, Lyrics, Settings, Minimize, Maximize, Restore, CloseWin };

struct ButtonAnimState {
    float hover = 0.0f;
    float press = 0.0f;
    float clickBounce = 0.0f;
    float repeatAnim = 0.0f;
    float diceToss = 0.0f;
    float shuffleAnim = 0.0f;
    float muteAnim = 0.0f;
    bool prevActive = false;
};
static std::unordered_map<ImGuiID, ButtonAnimState> s_btnAnimMap;

struct ToggleAnimState {
    float progress = 0.0f;
    float hover = 0.0f;
    float press = 0.0f;
    bool initialized = false;
};
static std::unordered_map<ImGuiID, ToggleAnimState> s_toggleAnimMap;

static bool AnimatedToggle(const char* strId, bool* v, const ImVec4& accent, float alpha = 1.0f) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID id = window->GetID(strId);
    ImVec2 pos = window->DC.CursorPos;
    const float w = 46.0f;
    const float h = 24.0f;
    const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    if (pressed) {
        *v = !(*v);
    }

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;

    ToggleAnimState& st = s_toggleAnimMap[id];
    if (!st.initialized) {
        st.progress = *v ? 1.0f : 0.0f;
        st.initialized = true;
    }

    float targetProg = *v ? 1.0f : 0.0f;
    float targetHover = hovered ? 1.0f : 0.0f;
    float targetPress = held ? 1.0f : 0.0f;

    st.progress += (targetProg - st.progress) * std::min(1.0f, 22.0f * dt);
    st.hover += (targetHover - st.hover) * std::min(1.0f, 18.0f * dt);
    st.press += (targetPress - st.press) * std::min(1.0f, 25.0f * dt);

    ImDrawList* dl = window->DrawList;
    float r = h * 0.5f;

    // Плавное перетекание цвета дорожки тумблера: тёмный графит -> яркий динамический акцент
    ImVec4 offBg(0.18f, 0.20f, 0.26f, 0.90f * alpha);
    ImVec4 onBg(accent.x, accent.y, accent.z, 0.92f * alpha);
    ImVec4 curBg(
        offBg.x + (onBg.x - offBg.x) * st.progress,
        offBg.y + (onBg.y - offBg.y) * st.progress,
        offBg.z + (onBg.z - offBg.z) * st.progress,
        offBg.w + (onBg.w - offBg.w) * st.progress
    );

    if (st.hover > 0.01f) {
        curBg.x = std::min(1.0f, curBg.x + 0.08f * st.hover);
        curBg.y = std::min(1.0f, curBg.y + 0.08f * st.hover);
        curBg.z = std::min(1.0f, curBg.z + 0.08f * st.hover);
    }

    // Мягкий неоновый ореол вокруг активного тумблера
    if (st.progress > 0.01f || st.hover > 0.01f) {
        float glowA = (st.progress * 0.35f + st.hover * 0.15f) * alpha;
        dl->AddRectFilled(
            ImVec2(bb.Min.x - 2.0f, bb.Min.y - 2.0f),
            ImVec2(bb.Max.x + 2.0f, bb.Max.y + 2.0f),
            ImColor(accent.x, accent.y, accent.z, glowA * 0.35f),
            r + 2.0f
        );
    }

    // Фоновая подложка дорожки
    dl->AddRectFilled(bb.Min, bb.Max, ImColor(curBg), r);

    // Тонкая окантовка дорожки
    ImVec4 borderCol(
        0.30f + (accent.x - 0.30f) * st.progress,
        0.34f + (accent.y - 0.34f) * st.progress,
        0.42f + (accent.z - 0.42f) * st.progress,
        (0.40f + 0.30f * st.hover) * alpha
    );
    dl->AddRect(bb.Min, bb.Max, ImColor(borderCol), r, 0, 1.0f);

    // Бегунок (Knob / Thumb)
    float knobRadius = r - 2.8f;
    float startX = bb.Min.x + r;
    float endX = bb.Max.x - r;
    float knobX = startX + (endX - startX) * st.progress;
    float knobY = bb.Min.y + r;

    // При зажатии бегунок слегка удлиняется (тактильный физический отклик)
    float stretch = st.press * 2.8f;
    float kLeft = knobX - knobRadius - (st.progress < 0.5f ? 0.0f : stretch);
    float kRight = knobX + knobRadius + (st.progress >= 0.5f ? 0.0f : stretch);
    ImVec2 kMin(kLeft, knobY - knobRadius);
    ImVec2 kMax(kRight, knobY + knobRadius);

    // Мягкая тень под бегунком
    dl->AddRectFilled(
        ImVec2(kMin.x, kMin.y + 1.2f),
        ImVec2(kMax.x, kMax.y + 1.5f),
        ImColor(0.0f, 0.0f, 0.0f, 0.38f * alpha),
        knobRadius
    );

    // Сам круглый бегунок
    dl->AddRectFilled(kMin, kMax, ImColor(0.98f, 0.99f, 1.0f, 0.98f * alpha), knobRadius);
    dl->AddRect(kMin, kMax, ImColor(1.0f, 1.0f, 1.0f, 0.80f * alpha), knobRadius, 0, 1.0f);

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    return pressed;
}

static bool IconButton(const char* idStr, IconType icon, ImVec2 size, bool activeState, const ImVec4& accent, float alpha, bool primary = false, float extraVal = 0.0f) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID id = window->GetID(idStr);
    ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
    double now = ImGui::GetTime();

    ButtonAnimState& st = s_btnAnimMap[id];
    float targetHover = hovered ? 1.0f : 0.0f;
    float targetPress = held ? 1.0f : 0.0f;

    // Лаконичные, быстрые и аккуратные переходы
    st.hover += (targetHover - st.hover) * std::min(1.0f, 18.0f * dt);
    st.press += (targetPress - st.press) * std::min(1.0f, 25.0f * dt);
    st.clickBounce = std::max(0.0f, st.clickBounce - 13.0f * dt);
    st.repeatAnim = std::max(0.0f, st.repeatAnim - 4.5f * dt);
    st.diceToss = std::max(0.0f, st.diceToss - 3.8f * dt);
    st.shuffleAnim = std::max(0.0f, st.shuffleAnim - 5.0f * dt);
    st.muteAnim = std::max(0.0f, st.muteAnim - 5.0f * dt);

    if (activeState && !st.prevActive && icon == IconType::Repeat) {
        st.repeatAnim = 1.0f;
    }
    if (activeState && icon == IconType::Dice && st.diceToss <= 0.001f) {
        st.diceToss = 1.0f;
    }
    st.prevActive = activeState;

    if (pressed) {
        st.clickBounce = 1.0f;
        if (icon == IconType::Repeat) {
            st.repeatAnim = 1.0f;
        } else if (icon == IconType::Dice) {
            st.diceToss = 1.0f;
        } else if (icon == IconType::Shuffle) {
            st.shuffleAnim = 1.0f;
        } else if (icon == IconType::Volume || icon == IconType::Mute) {
            st.muteAnim = 1.0f;
        }
    }

    ImDrawList* dl = window->DrawList;
    ImVec2 c(std::floor((bb.Min.x + bb.Max.x) * 0.5f), std::floor((bb.Min.y + bb.Max.y) * 0.5f));

    bool isCloseBtn = (icon == IconType::CloseWin);

    // Микро-смещения для приятного тактильного отклика (аккуратные и сдержанные)
    if (icon == IconType::Prev) {
        c.x -= 1.2f * st.hover;
    } else if (icon == IconType::Next) {
        c.x += 1.2f * st.hover;
    }

    // Физика подбрасывания кубика (Dice Toss) для кнопки рулетки
    if (icon == IconType::Dice && st.diceToss > 0.001f) {
        float p = 1.0f - st.diceToss;
        // Параболическая траектория подбрасывания вверх и приземления
        float tossY = -7.5f * std::sin(p * 3.14159265f);
        // Микро-отскок при приземлении
        if (p > 0.82f) {
            tossY += std::sin((p - 0.82f) / 0.18f * 3.14159265f) * 1.5f;
        }
        c.y += tossY;
    }

    // Расчёт коэффициента масштабирования (scale) с лаконичной физикой
    float bounce = std::sin(st.clickBounce * 3.14159265f) * 0.035f;
    float scale = 1.0f + st.hover * 0.045f - st.press * 0.06f + bounce;
    if (primary) scale *= 1.0f + st.hover * 0.02f;

    // Угол поворота иконки (только аккуратные наклоны, без бесконечной прокрутки)
    float iconAngle = 0.0f;
    if (icon == IconType::Dice) {
        if (st.diceToss > 0.001f) {
            float p = 1.0f - st.diceToss;
            // Естественный наклон кости в полёте (без вращения пропеллером)
            iconAngle = std::sin(p * 6.2831853f) * 0.20f;
        } else if (hovered) {
            iconAngle = std::sin(static_cast<float>(now) * 6.0f) * 0.06f;
        }
    } else if (icon == IconType::Settings) {
        iconAngle = g_settingsGearRotation + st.hover * 0.15f;
    } else if (icon == IconType::CloseWin) {
        iconAngle = st.hover * 0.785398f; // Аккуратный поворот на 45 градусов
    }

    // Мягкий аккуратный ореол за кнопкой
    if (st.hover > 0.01f || activeState) {
        float glowR = (size.y * 0.5f) * (1.0f + st.hover * 0.20f);
        ImVec4 gCol = activeState
            ? ImVec4(accent.x, accent.y, accent.z, 0.22f * alpha)
            : (isCloseBtn ? ImVec4(0.92f, 0.20f, 0.25f, 0.20f * st.hover * alpha)
                          : ImVec4(accent.x, accent.y, accent.z, 0.14f * st.hover * alpha));
        dl->AddCircleFilled(c, glowR, ImColor(gCol), 24);
    }

    // Фоновая подложка кнопки с анимированным масштабированием
    ImVec2 curMin(c.x - size.x * 0.5f * scale, c.y - size.y * 0.5f * scale);
    ImVec2 curMax(c.x + size.x * 0.5f * scale, c.y + size.y * 0.5f * scale);

    if (primary) {
        ImVec4 bg = held ? ImVec4(0.88f, 0.90f, 0.95f, alpha)
                  : (hovered ? ImVec4(1.0f, 1.0f, 1.0f, alpha) : ImVec4(0.95f, 0.96f, 0.98f, 0.95f * alpha));
        dl->AddCircleFilled(c, size.y * 0.5f * scale, ImColor(bg), 32);
    } else {
        float bgAlpha = held ? 0.32f : (hovered ? 0.22f : (activeState ? 0.16f : 0.08f * st.hover));
        ImVec4 bg = isCloseBtn && (hovered || held)
            ? (held ? ImVec4(0.82f, 0.18f, 0.24f, 0.95f * alpha) : ImVec4(0.92f, 0.24f, 0.30f, 0.88f * alpha))
            : (activeState ? ImVec4(accent.x, accent.y, accent.z, bgAlpha * alpha * 1.55f)
                           : ImVec4(1.0f, 1.0f, 1.0f, bgAlpha * alpha));
        if (bgAlpha > 0.005f || isCloseBtn) {
            dl->AddRectFilled(curMin, curMax, ImColor(bg), size.y * 0.42f * scale);
        }
    }

    ImU32 fg = primary
        ? ImColor(ImVec4(0.07f, 0.08f, 0.11f, alpha))
        : (activeState ? ImColor(ImVec4(accent.x, accent.y, accent.z, alpha))
                       : (isCloseBtn && (hovered || held)
                           ? ImColor(ImVec4(1.0f, 1.0f, 1.0f, alpha))
                           : ImColor(ImVec4(0.92f, 0.94f, 0.98f, (hovered ? 1.0f : 0.80f) * alpha))));

    auto xform = [&](const ImVec2& pt) -> ImVec2 {
        if (std::abs(iconAngle) < 0.001f && std::abs(scale - 1.0f) < 0.001f) {
            return pt;
        }
        float cosA = std::cos(iconAngle);
        float sinA = std::sin(iconAngle);
        float dx = (pt.x - c.x) * scale;
        float dy = (pt.y - c.y) * scale;
        return ImVec2(c.x + dx * cosA - dy * sinA, c.y + dx * sinA + dy * cosA);
    };

    auto drawTransformedRect = [&](ImVec2 minP, ImVec2 maxP, ImU32 col, float rounding = 0.0f) {
        if (std::abs(iconAngle) < 0.001f) {
            dl->AddRectFilled(xform(minP), xform(maxP), col, rounding * scale);
        } else {
            ImVec2 p0 = xform(minP);
            ImVec2 p1 = xform(ImVec2(maxP.x, minP.y));
            ImVec2 p2 = xform(maxP);
            ImVec2 p3 = xform(ImVec2(minP.x, maxP.y));
            dl->AddQuadFilled(p0, p1, p2, p3, col);
        }
    };

    auto drawTransformedRectStroke = [&](ImVec2 minP, ImVec2 maxP, ImU32 col, float rounding = 0.0f, float thick = 1.0f) {
        if (std::abs(iconAngle) < 0.001f) {
            dl->AddRect(xform(minP), xform(maxP), col, rounding * scale, 0, thick * scale);
        } else {
            ImVec2 p0 = xform(minP);
            ImVec2 p1 = xform(ImVec2(maxP.x, minP.y));
            ImVec2 p2 = xform(maxP);
            ImVec2 p3 = xform(ImVec2(minP.x, maxP.y));
            dl->AddQuad(p0, p1, p2, p3, col, thick * scale);
        }
    };

    switch (icon) {
        case IconType::Play: {
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 3.5f, c.y - 6.5f)),
                xform(ImVec2(c.x - 3.5f, c.y + 6.5f)),
                xform(ImVec2(c.x + 6.5f, c.y)),
                fg
            );
            break;
        }
        case IconType::Pause: {
            drawTransformedRect(ImVec2(c.x - 5.5f, c.y - 6.0f), ImVec2(c.x - 2.0f, c.y + 6.0f), fg, 1.2f);
            drawTransformedRect(ImVec2(c.x + 2.0f, c.y - 6.0f), ImVec2(c.x + 5.5f, c.y + 6.0f), fg, 1.2f);
            break;
        }
        case IconType::Prev: {
            drawTransformedRect(ImVec2(c.x - 6.5f, c.y - 5.5f), ImVec2(c.x - 4.5f, c.y + 5.5f), fg, 1.0f);
            dl->AddTriangleFilled(
                xform(ImVec2(c.x + 5.5f, c.y - 5.5f)),
                xform(ImVec2(c.x + 5.5f, c.y + 5.5f)),
                xform(ImVec2(c.x - 2.0f, c.y)),
                fg
            );
            break;
        }
        case IconType::Next: {
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 5.5f, c.y - 5.5f)),
                xform(ImVec2(c.x - 5.5f, c.y + 5.5f)),
                xform(ImVec2(c.x + 2.0f, c.y)),
                fg
            );
            drawTransformedRect(ImVec2(c.x + 4.5f, c.y - 5.5f), ImVec2(c.x + 6.5f, c.y + 5.5f), fg, 1.0f);
            break;
        }
        case IconType::Shuffle: {
            float shAnim = st.shuffleAnim;
            float shP = (shAnim > 0.001f) ? std::sin((1.0f - shAnim) * 3.14159265f) : 0.0f;
            float flex = shP * 1.5f;
            float dart = shP * 1.8f;

            // Две элегантно перекрещивающиеся линии с плавным тактильным изгибом при нажатии
            dl->AddLine(xform(ImVec2(c.x - 6.5f, c.y - 4.0f)), xform(ImVec2(c.x + 4.5f + dart, c.y + 4.0f + flex)), fg, 1.8f * scale);
            dl->AddLine(xform(ImVec2(c.x - 6.5f, c.y + 4.0f)), xform(ImVec2(c.x + 4.5f + dart, c.y - 4.0f - flex)), fg, 1.8f * scale);

            // Стрелки на концах перекрестий
            float topArrX = c.x + 4.5f + dart;
            float topArrY = c.y - 4.0f - flex;
            dl->AddTriangleFilled(
                xform(ImVec2(topArrX - 3.2f, topArrY - 2.8f)),
                xform(ImVec2(topArrX + 2.4f, topArrY)),
                xform(ImVec2(topArrX - 3.2f, topArrY + 2.8f)),
                fg
            );

            float botArrX = c.x + 4.5f + dart;
            float botArrY = c.y + 4.0f + flex;
            dl->AddTriangleFilled(
                xform(ImVec2(botArrX - 3.2f, botArrY - 2.8f)),
                xform(ImVec2(botArrX + 2.4f, botArrY)),
                xform(ImVec2(botArrX - 3.2f, botArrY + 2.8f)),
                fg
            );
            break;
        }
        case IconType::Repeat: {
            // Элегантная закруглённая контурная петля
            drawTransformedRectStroke(ImVec2(c.x - 6.5f, c.y - 4.5f), ImVec2(c.x + 6.5f, c.y + 4.5f), fg, 3.8f, 1.8f);

            // Анимация стрелочек внутри кнопки при нажатии (лаконичный импульсный сдвиг по циклу)
            float shift = 0.0f;
            float arrowScale = 1.0f;
            if (st.repeatAnim > 0.001f) {
                float p = 1.0f - st.repeatAnim;
                float impulse = std::sin(p * 3.14159265f);
                shift = impulse * 3.2f;
                arrowScale = 1.0f + impulse * 0.20f;
            }

            // Стрелка сверху, указывающая направо (по часовой стрелке)
            float topX = c.x + 4.0f + shift;
            float topY = c.y - 4.5f;
            dl->AddTriangleFilled(
                xform(ImVec2(topX - 3.5f * arrowScale, topY - 3.0f * arrowScale)),
                xform(ImVec2(topX + 1.2f * arrowScale, topY)),
                xform(ImVec2(topX - 3.5f * arrowScale, topY + 3.0f * arrowScale)),
                fg
            );

            // Стрелка снизу, указывающая налево
            float botX = c.x - 4.0f - shift;
            float botY = c.y + 4.5f;
            dl->AddTriangleFilled(
                xform(ImVec2(botX + 3.5f * arrowScale, botY - 3.0f * arrowScale)),
                xform(ImVec2(botX - 1.2f * arrowScale, botY)),
                xform(ImVec2(botX + 3.5f * arrowScale, botY + 3.0f * arrowScale)),
                fg
            );
            break;
        }
        case IconType::Volume: {
            float mAnim = st.muteAnim;
            float mPulse = (mAnim > 0.001f) ? std::sin((1.0f - mAnim) * 3.14159265f) : 0.0f;
            float recoil = -mPulse * 1.2f;

            // Корпус динамика (базовый прямоугольник + конус рупора)
            drawTransformedRect(ImVec2(c.x - 6.5f + recoil, c.y - 2.5f), ImVec2(c.x - 3.2f + recoil, c.y + 2.5f), fg, 0.8f);
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 3.2f + recoil, c.y - 2.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y - 5.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y + 5.5f)),
                fg
            );
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 3.2f + recoil, c.y - 2.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y + 5.5f)),
                xform(ImVec2(c.x - 3.2f + recoil, c.y + 2.5f)),
                fg
            );

            // Динамические звуковые волны (1, 2 или 3 дуги в зависимости от громкости)
            float volVal = extraVal;
            float rip = mPulse * 0.8f;

            // 1-я волна: при громкости > 0%
            if (volVal > 0.01f) {
                dl->AddBezierQuadratic(
                    xform(ImVec2(c.x + 1.8f + rip, c.y - 3.0f)),
                    xform(ImVec2(c.x + 3.6f + rip, c.y)),
                    xform(ImVec2(c.x + 1.8f + rip, c.y + 3.0f)),
                    fg, 1.5f * scale
                );
            }
            // 2-я волна: при громкости > 33%
            if (volVal > 0.33f) {
                dl->AddBezierQuadratic(
                    xform(ImVec2(c.x + 3.8f + rip * 1.3f, c.y - 4.8f)),
                    xform(ImVec2(c.x + 6.2f + rip * 1.3f, c.y)),
                    xform(ImVec2(c.x + 3.8f + rip * 1.3f, c.y + 4.8f)),
                    fg, 1.5f * scale
                );
            }
            // 3-я волна: при высокой громкости > 66%
            if (volVal > 0.66f) {
                dl->AddBezierQuadratic(
                    xform(ImVec2(c.x + 5.8f + rip * 1.6f, c.y - 6.5f)),
                    xform(ImVec2(c.x + 8.8f + rip * 1.6f, c.y)),
                    xform(ImVec2(c.x + 5.8f + rip * 1.6f, c.y + 6.5f)),
                    fg, 1.5f * scale
                );
            }
            break;
        }
        case IconType::Mute: {
            float mAnim = st.muteAnim;
            float mPulse = (mAnim > 0.001f) ? std::sin((1.0f - mAnim) * 3.14159265f) : 0.0f;
            float recoil = -mPulse * 1.2f;

            // Корпус динамика
            drawTransformedRect(ImVec2(c.x - 6.5f + recoil, c.y - 2.5f), ImVec2(c.x - 3.2f + recoil, c.y + 2.5f), fg, 0.8f);
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 3.2f + recoil, c.y - 2.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y - 5.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y + 5.5f)),
                fg
            );
            dl->AddTriangleFilled(
                xform(ImVec2(c.x - 3.2f + recoil, c.y - 2.5f)),
                xform(ImVec2(c.x - 0.5f + recoil, c.y + 5.5f)),
                xform(ImVec2(c.x - 3.2f + recoil, c.y + 2.5f)),
                fg
            );

            // Чёткий аккуратный крестик (mute 'x')
            float xSz = 2.4f * (1.0f + mPulse * 0.25f);
            float xMid = c.x + 4.2f;
            dl->AddLine(xform(ImVec2(xMid - xSz, c.y - xSz)), xform(ImVec2(xMid + xSz, c.y + xSz)), fg, 1.7f * scale);
            dl->AddLine(xform(ImVec2(xMid + xSz, c.y - xSz)), xform(ImVec2(xMid - xSz, c.y + xSz)), fg, 1.7f * scale);
            break;
        }
        case IconType::Dice: {
            drawTransformedRectStroke(ImVec2(c.x - 7.0f, c.y - 7.0f), ImVec2(c.x + 7.0f, c.y + 7.0f), fg, 3.0f, 1.8f);

            // Определение грани кости (при подбрасывании грани динамично сменяются в воздухе)
            int pipFace = 5;
            if (st.diceToss > 0.001f) {
                float p = 1.0f - st.diceToss;
                if (p < 0.82f) {
                    int phase = static_cast<int>(p * 18.0f) % 4;
                    if (phase == 0) pipFace = 2;
                    else if (phase == 1) pipFace = 4;
                    else if (phase == 2) pipFace = 3;
                    else pipFace = 6;
                }
            }

            auto drawDot = [&](float dx, float dy) {
                dl->AddCircleFilled(xform(ImVec2(c.x + dx, c.y + dy)), 1.35f * scale, fg);
            };

            if (pipFace == 1 || pipFace == 3 || pipFace == 5) {
                drawDot(0.0f, 0.0f);
            }
            if (pipFace >= 2) {
                drawDot( 3.2f, -3.2f);
                drawDot(-3.2f,  3.2f);
            }
            if (pipFace >= 4) {
                drawDot(-3.2f, -3.2f);
                drawDot( 3.2f,  3.2f);
            }
            if (pipFace == 6) {
                drawDot(-3.2f,  0.0f);
                drawDot( 3.2f,  0.0f);
            }
            break;
        }
        case IconType::Lyrics: {
            drawTransformedRectStroke(ImVec2(c.x - 6.5f, c.y - 7.0f), ImVec2(c.x + 6.5f, c.y + 7.0f), fg, 2.2f, 1.6f);
            dl->AddLine(xform(ImVec2(c.x - 3.5f, c.y - 3.2f)), xform(ImVec2(c.x + 3.5f, c.y - 3.2f)), fg, 1.6f * scale);
            dl->AddLine(xform(ImVec2(c.x - 3.5f, c.y + 0.0f)), xform(ImVec2(c.x + 2.0f, c.y + 0.0f)), fg, 1.6f * scale);
            dl->AddLine(xform(ImVec2(c.x - 3.5f, c.y + 3.2f)), xform(ImVec2(c.x + 3.5f, c.y + 3.2f)), fg, 1.6f * scale);
            break;
        }
        case IconType::Settings: {
            float rOuter = 5.2f;
            float rInner = 2.4f;
            dl->AddCircle(c, rOuter * scale, fg, 20, 1.6f * scale);
            dl->AddCircleFilled(c, rInner * scale, ImColor(ImVec4(accent.x, accent.y, accent.z, (activeState ? 0.95f : 0.35f) * alpha)), 16);

            for (int k = 0; k < 6; ++k) {
                float toothAngle = k * (3.14159265f / 3.0f);
                float ct = std::cos(toothAngle);
                float stA = std::sin(toothAngle);
                ImVec2 tBase(c.x + ct * (rOuter - 0.5f), c.y + stA * (rOuter - 0.5f));
                ImVec2 tTip (c.x + ct * (rOuter + 2.6f), c.y + stA * (rOuter + 2.6f));
                dl->AddLine(xform(tBase), xform(tTip), fg, 2.4f * scale);
            }
            break;
        }
        case IconType::Minimize: {
            dl->AddLine(xform(ImVec2(c.x - 5.5f, c.y + 1.0f)), xform(ImVec2(c.x + 5.5f, c.y + 1.0f)), fg, 1.8f * scale);
            break;
        }
        case IconType::Maximize: {
            drawTransformedRectStroke(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), fg, 1.5f, 1.6f);
            break;
        }
        case IconType::Restore: {
            drawTransformedRectStroke(ImVec2(c.x - 3.0f, c.y - 5.5f), ImVec2(c.x + 5.5f, c.y + 3.0f), fg, 1.2f, 1.5f);
            drawTransformedRectStroke(ImVec2(c.x - 5.5f, c.y - 3.0f), ImVec2(c.x + 3.0f, c.y + 5.5f), fg, 1.2f, 1.5f);
            break;
        }
        case IconType::CloseWin: {
            dl->AddLine(xform(ImVec2(c.x - 5.0f, c.y - 5.0f)), xform(ImVec2(c.x + 5.0f, c.y + 5.0f)), fg, 1.8f * scale);
            dl->AddLine(xform(ImVec2(c.x + 5.0f, c.y - 5.0f)), xform(ImVec2(c.x - 5.0f, c.y + 5.0f)), fg, 1.8f * scale);
            break;
        }
    }

    return pressed;
}

static int findTrackByYmId(const std::string& ymId) {
    if (ymId.empty()) return -1;
    std::string needle = "ym_" + ymId + ".mp3";
    std::wstring wNeedle = MetadataParser::utf8ToWide(needle);
    for (size_t i = 0; i < g_playlist.size(); ++i) {
        if (g_playlist[i].widePath.find(wNeedle) != std::wstring::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool DrawYandexAuthButton(const char* idStr, ImVec2 size, bool isAuthenticated, const std::string& userName, float alpha) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const ImGuiID id = window->GetID(idStr);
    ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    ImDrawList* dl = window->DrawList;
    ImVec2 c((bb.Min.x + bb.Max.x) * 0.5f, (bb.Min.y + bb.Max.y) * 0.5f);

    float bgAlpha = held ? 0.32f : (hovered ? 0.22f : 0.12f);
    ImVec4 bgCol = ImVec4(1.0f, 1.0f, 1.0f, bgAlpha * alpha);
    ImVec4 borderCol = hovered ? ImVec4(1.0f, 1.0f, 1.0f, 0.32f * alpha)
                               : ImVec4(1.0f, 1.0f, 1.0f, 0.14f * alpha);

    float rounding = size.y * 0.44f;
    dl->AddRectFilled(bb.Min, bb.Max, ImColor(bgCol), rounding);
    dl->AddRect(bb.Min, bb.Max, ImColor(borderCol), rounding, 0, 1.0f);

    // Фирменный круг Яндекс (красный) по центру кнопки
    ImVec2 iconCenter(c.x, c.y);
    float iconRadius = 8.5f;
    ImVec4 ymRed = ImVec4(0.988f, 0.235f, 0.118f, 1.0f * alpha);
    dl->AddCircleFilled(iconCenter, iconRadius, ImColor(ymRed), 24);

    // Буква "Я" внутри круга
    if (g_fontMainBold) {
        const char* letter = "Я";
        ImVec2 textSz = g_fontMainBold->CalcTextSizeA(12.5f, FLT_MAX, -1.0f, letter);
        dl->AddText(g_fontMainBold, 12.5f,
                    ImVec2(iconCenter.x - textSz.x * 0.5f, iconCenter.y - textSz.y * 0.5f - 0.5f),
                    ImColor(1.0f, 1.0f, 1.0f, 1.0f * alpha), letter);
    }

    // Мини-индикатор статуса авторизации в правом нижнем углу кнопки
    ImVec2 badgePos(bb.Max.x - 5.5f, bb.Max.y - 5.5f);
    float badgeR = 3.8f;

    // Тёмный контур подложки вокруг индикатора
    dl->AddCircleFilled(badgePos, badgeR + 1.2f, ImColor(0.06f, 0.07f, 0.10f, 1.0f * alpha), 16);

    if (isAuthenticated) {
        // Зелёная точка со стилизованной векторной галочкой
        ImVec4 greenCol = ImVec4(0.18f, 0.85f, 0.44f, 1.0f * alpha);
        dl->AddCircleFilled(badgePos, badgeR, ImColor(greenCol), 16);

        ImU32 checkCol = ImColor(1.0f, 1.0f, 1.0f, 1.0f * alpha);
        dl->AddLine(ImVec2(badgePos.x - 1.6f, badgePos.y + 0.1f),
                    ImVec2(badgePos.x - 0.3f, badgePos.y + 1.4f), checkCol, 1.2f);
        dl->AddLine(ImVec2(badgePos.x - 0.3f, badgePos.y + 1.4f),
                    ImVec2(badgePos.x + 1.6f, badgePos.y - 1.2f), checkCol, 1.2f);
    } else {
        // Красная точка со стилизованным векторным крестиком
        ImVec4 redCol = ImVec4(0.88f, 0.28f, 0.28f, 0.95f * alpha);
        dl->AddCircleFilled(badgePos, badgeR, ImColor(redCol), 16);

        ImU32 crossCol = ImColor(1.0f, 1.0f, 1.0f, 0.95f * alpha);
        dl->AddLine(ImVec2(badgePos.x - 1.2f, badgePos.y - 1.2f),
                    ImVec2(badgePos.x + 1.2f, badgePos.y + 1.2f), crossCol, 1.1f);
        dl->AddLine(ImVec2(badgePos.x - 1.2f, badgePos.y + 1.2f),
                    ImVec2(badgePos.x + 1.2f, badgePos.y - 1.2f), crossCol, 1.1f);
    }

    if (hovered) {
        if (isAuthenticated) {
            std::string tip = "Яндекс Музыка: " + (userName.empty() ? std::string("Подключено") : userName) + "\nНажмите для управления аккаунтом";
            ImGui::SetTooltip("%s", tip.c_str());
        } else {
            ImGui::SetTooltip("Яндекс Музыка: Не авторизован\nНажмите для входа в аккаунт");
        }
    }

    return pressed;
}

static bool DrawSearchSourceToggle(SearchSource& source, const ImVec4& accent, float itemH) {
    bool changed = false;
    const float halfW = 72.0f;
    const float totalW = halfW * 2.0f; // 144.0f
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushID("##SearchSourceToggleWidget");
    bool clicked = ImGui::InvisibleButton("##ToggleBtn", ImVec2(totalW, itemH));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    float mouseRelX = ImGui::GetIO().MousePos.x - p.x;
    bool hoverLeft = hovered && (mouseRelX < halfW);
    bool hoverRight = hovered && (mouseRelX >= halfW);

    if (clicked) {
        if (mouseRelX < halfW) {
            if (source != SearchSource::Local) {
                source = SearchSource::Local;
                changed = true;
            }
        } else {
            if (source != SearchSource::Yandex) {
                source = SearchSource::Yandex;
                changed = true;
            }
        }
    }

    static float s_toggleAnim = 0.0f;
    float targetAnim = (source == SearchSource::Yandex) ? 1.0f : 0.0f;
    float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);
    s_toggleAnim += (targetAnim - s_toggleAnim) * std::clamp(dt * 22.0f, 0.0f, 1.0f);

    float rounding = itemH * 0.44f;
    ImVec2 pMax(p.x + totalW, p.y + itemH);

    // Фоновая подложка капсулы переключателя
    dl->AddRectFilled(p, pMax, ImColor(0.06f, 0.07f, 0.10f, 0.90f), rounding);
    dl->AddRect(p, pMax, ImColor(1.0f, 1.0f, 1.0f, 0.12f), rounding, 0, 1.0f);

    // Скользящая акцентная плашка переключателя
    const float pad = 2.0f;
    const float pillW = halfW - pad * 2.0f;
    const float pillH = itemH - pad * 2.0f;
    float pillX = p.x + pad + s_toggleAnim * halfW;
    float pillY = p.y + pad;
    ImVec2 pillMin(pillX, pillY);
    ImVec2 pillMax(pillX + pillW, pillY + pillH);
    float pillRounding = pillH * 0.44f;

    ImU32 pillBgCol = ImColor(accent.x, accent.y, accent.z, 0.38f);
    ImU32 pillBorderCol = ImColor(accent.x, accent.y, accent.z, 0.65f);
    dl->AddRectFilled(pillMin, pillMax, pillBgCol, pillRounding);
    dl->AddRect(pillMin, pillMax, pillBorderCol, pillRounding, 0, 1.0f);

    // Мягкий блик на верхней кромке плашки для объёма
    dl->AddLine(ImVec2(pillMin.x + pillRounding * 0.6f, pillMin.y + 1.0f),
                ImVec2(pillMax.x - pillRounding * 0.6f, pillMin.y + 1.0f),
                ImColor(1.0f, 1.0f, 1.0f, 0.25f), 1.0f);

    // Рендер надписей строго математически по центрам обеих половинок
    ImFont* font = (g_boldInterfaceFont && g_fontMainBold) ? g_fontMainBold : (g_fontMain ? g_fontMain : ImGui::GetFont());
    const char* label1 = "Локально";
    const char* label2 = "Яндекс";
    ImVec2 sz1 = font->CalcTextSizeA(14.0f, FLT_MAX, -1.0f, label1);
    ImVec2 sz2 = font->CalcTextSizeA(14.0f, FLT_MAX, -1.0f, label2);

    ImVec2 c1(p.x + halfW * 0.5f, p.y + itemH * 0.5f);
    ImVec2 c2(p.x + halfW + halfW * 0.5f, p.y + itemH * 0.5f);

    ImVec2 tPos1(std::round(c1.x - sz1.x * 0.5f), std::round(c1.y - sz1.y * 0.5f - 0.5f));
    ImVec2 tPos2(std::round(c2.x - sz2.x * 0.5f), std::round(c2.y - sz2.y * 0.5f - 0.5f));

    float activeRatio1 = 1.0f - s_toggleAnim;
    float activeRatio2 = s_toggleAnim;

    ImVec4 whiteCol(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 mutedCol(0.60f, 0.65f, 0.75f, 0.85f);
    if (hoverLeft && source != SearchSource::Local) mutedCol = ImVec4(0.80f, 0.85f, 0.95f, 0.95f);
    ImVec4 col1 = ImVec4(
        mutedCol.x + (whiteCol.x - mutedCol.x) * activeRatio1,
        mutedCol.y + (whiteCol.y - mutedCol.y) * activeRatio1,
        mutedCol.z + (whiteCol.z - mutedCol.z) * activeRatio1,
        mutedCol.w + (whiteCol.w - mutedCol.w) * activeRatio1
    );

    ImVec4 mutedCol2(0.60f, 0.65f, 0.75f, 0.85f);
    if (hoverRight && source != SearchSource::Yandex) mutedCol2 = ImVec4(0.80f, 0.85f, 0.95f, 0.95f);
    ImVec4 col2 = ImVec4(
        mutedCol2.x + (whiteCol.x - mutedCol2.x) * activeRatio2,
        mutedCol2.y + (whiteCol.y - mutedCol2.y) * activeRatio2,
        mutedCol2.z + (whiteCol.z - mutedCol2.z) * activeRatio2,
        mutedCol2.w + (whiteCol.w - mutedCol2.w) * activeRatio2
    );

    dl->AddText(font, 14.0f, tPos1, ImColor(col1), label1);
    dl->AddText(font, 14.0f, tPos2, ImColor(col2), label2);

    return changed;
}

// ------------------ РЕНДЕР ДЕБАГ-МОНИТОРА ПРОИЗВОДИТЕЛЬНОСТИ ------------------
static void renderDebugOverlay(bool isActivelyAnimating, const glm::vec3& accentColor, int windowW, int windowH, AudioEngine& audio) {
    if (!g_showDebugOverlay) return;

    if (g_debugOverlayFirstOpen) {
        // Располагаем аккуратно в правом верхнем углу прямо под кнопками окна
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(windowW) - 248.0f, 72.0f), ImGuiCond_Appearing);
        g_debugOverlayFirstOpen = false;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.10f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.99f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings;

    bool overlayOpen = g_showDebugOverlay;
    if (ImGui::Begin("Дебаг-монитор##HUD", &overlayOpen, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();

        // Пульсирующая статусная точка активности
        float pulse = 0.72f + 0.28f * std::sin(static_cast<float>(glfwGetTime()) * 4.5f);
        ImVec4 dotCol = isActivelyAnimating
            ? ImVec4(0.20f, 0.88f, 0.55f, pulse) // Изумрудно-зелёный при анимации / скролле (100 FPS)
            : (g_lyricsInspectMode
                ? ImVec4(0.35f, 0.78f, 1.0f, pulse) // Небесно-голубой при показе текста / караоке (60 FPS)
                : ImVec4(0.42f, 0.58f, 0.88f, 0.75f)); // Спокойный синий в глубоком покое (30 FPS)
        dl->AddCircleFilled(ImVec2(cp.x + 5.0f, cp.y + 8.0f), 4.2f, ImColor(dotCol));

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        ImGui::TextColored(ImVec4(0.85f, 0.89f, 0.98f, 1.0f), "ДЕБАГ ТЕЛЕМЕТРИЯ");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 1. Кадровая частота (FPS и время кадра)
        ImVec4 fpsCol = (g_telemetryFps >= 50.0f) ? ImVec4(0.35f, 0.92f, 1.0f, 1.0f) : ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "Кадры:");
        ImGui::SameLine(92.0f);
        ImGui::TextColored(fpsCol, "%.1f FPS", g_telemetryFps);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.50f, 0.55f, 0.65f, 1.0f), "(%.1f ms)", g_telemetryFrameMs);

        // 2. Нагрузка ЦПУ (по официальной формуле Windows Task Manager: user+kernel delta / wall delta * cores)
        ImVec4 cpuCol = (g_telemetryCpuPercent < 1.0f) ? ImVec4(0.30f, 0.92f, 0.50f, 1.0f)
                      : (g_telemetryCpuPercent < 5.0f) ? ImVec4(1.0f, 0.85f, 0.30f, 1.0f)
                      : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "ЦПУ (ДЗ):");
        ImGui::SameLine(92.0f);
        ImGui::TextColored(cpuCol, "%.1f %%", g_telemetryCpuPercent);

        // 3. Нагрузка ГПУ (время выполнения 3D-пакетов silicon GPU, точно как в Диспетчере задач 3D Engine)
        ImVec4 gpuCol = (g_telemetryGpuPercent < 1.0f) ? ImVec4(0.30f, 0.92f, 0.50f, 1.0f)
                      : (g_telemetryGpuPercent < 5.0f) ? ImVec4(1.0f, 0.85f, 0.30f, 1.0f)
                      : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "ГПУ (3D):");
        ImGui::SameLine(92.0f);
        ImGui::TextColored(gpuCol, "%.1f %%", g_telemetryGpuPercent);

        // 4. Память (Working Set / Рабочий набор процесса в ОЗУ)
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "Память:");
        ImGui::SameLine(92.0f);
        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.96f, 1.0f), "%.1f МБ", g_telemetryRamMb);

        // 5. Текущий адаптивный режим частоты
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "Режим:");
        ImGui::SameLine(92.0f);
        if (isActivelyAnimating) {
            ImGui::TextColored(ImVec4(0.20f, 0.90f, 0.55f, 1.0f), "120 FPS (Активен)");
        } else if (g_lyricsInspectMode || g_showSettingsWindow) {
            ImGui::TextColored(ImVec4(0.35f, 0.78f, 1.0f, 1.0f), "60 FPS (Текст)");
        } else {
            ImGui::TextColored(ImVec4(0.50f, 0.65f, 0.85f, 1.0f), "30 FPS (Покой)");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.60f, 1.0f), "Горячая клавиша: F3");
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    if (g_showDebugOverlay != overlayOpen) {
        g_showDebugOverlay = overlayOpen;
        saveSettingsToDisk(audio);
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);

    // Включаем Per-Monitor V2 DPI awareness ДО инициализации GLFW и создания окна:
    // Это гарантирует, что Windows DWM никогда не будет растягивать и замыливать окно
    // билинейным апскейлингом в оконном режиме на мониторах с масштабированием 125%/150%!
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(HANDLE);
        auto setDpiAwarenessContext = (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiAwarenessContext) {
            setDpiAwarenessContext((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    if (!glfwInit()) {
        timeEndPeriod(1);
        return -1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1380, 800, "Jewel Box Player", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        timeEndPeriod(1);
        return -1;
    }

    HWND hwnd = glfwGetWin32Window(window);
    applyWindows11FrameStyle(hwnd);
    SmtcManager::instance().init(hwnd);
    YandexMusicManager::instance().init(getExeDir());

    if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) {
            glfwSetWindowPos(window, (mode->width - 1380) / 2, (mode->height - 800) / 2);
            if (mode->refreshRate > 0) {
                g_monitorRefreshRate = mode->refreshRate;
            }
        }
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, dropCallback);
    glfwSetScrollCallback(window, scrollCallback);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glEnable(GL_MULTISAMPLE);

    glClearColor(0.04f, 0.045f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glfwSwapBuffers(window);
    glfwShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    static ImVector<ImWchar> glyphRanges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar extraPunctuation[] = {
        0x00A0, 0x00FF, // Latin-1 Supplement (including «, », ±)
        0x2000, 0x206F, // General Punctuation (—, –, …, •, etc.)
        0x2100, 0x218F, // Letterlike Symbols (№, etc.)
        0x2190, 0x21FF, // Arrows (←, ↑, →, ↓, etc.)
        0x25A0, 0x25FF, // Geometric Shapes (■, ▶, ▼, ▲, ●, etc.)
        0x2600, 0x26FF, // Misc Symbols (♪, ♫, ⚙, etc.)
        0x2700, 0x27BF, // Dingbats (✓, ✕, ✗, etc.)
        0
    };
    builder.AddRanges(extraPunctuation);
    builder.BuildRanges(&glyphRanges);

    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* fontBoldPath = "C:\\Windows\\Fonts\\segoeuib.ttf";
    const char* fontSemiboldPath = "C:\\Windows\\Fonts\\seguisb.ttf";

    if (fs::exists(fontPath)) {
        const char* bPath = fs::exists(fontBoldPath) ? fontBoldPath : fontPath;
        const char* sbPath = fs::exists(fontSemiboldPath) ? fontSemiboldPath : bPath;

        // Базовые шрифты (оконный режим ~800p)
        g_fontMain = io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr, glyphRanges.Data);
        g_fontMainBold = io.Fonts->AddFontFromFileTTF(bPath, 16.0f, nullptr, glyphRanges.Data);
        g_fontTitleBold = io.Fonts->AddFontFromFileTTF(bPath, 23.0f, nullptr, glyphRanges.Data);
        g_fontArtistMedium = io.Fonts->AddFontFromFileTTF(sbPath, 17.0f, nullptr, glyphRanges.Data);
        g_fontLyricsRegular = io.Fonts->AddFontFromFileTTF(fontPath, 24.0f, nullptr, glyphRanges.Data);
        g_fontKaraokeBold = io.Fonts->AddFontFromFileTTF(bPath, 30.0f, nullptr, glyphRanges.Data);

        // Средние шрифты (1080p Fullscreen)
        g_fontMainMed = io.Fonts->AddFontFromFileTTF(fontPath, 19.0f, nullptr, glyphRanges.Data);
        g_fontTitleMed = io.Fonts->AddFontFromFileTTF(bPath, 28.0f, nullptr, glyphRanges.Data);
        g_fontArtistMed = io.Fonts->AddFontFromFileTTF(sbPath, 20.0f, nullptr, glyphRanges.Data);
        g_fontLyricsMed = io.Fonts->AddFontFromFileTTF(fontPath, 29.0f, nullptr, glyphRanges.Data);
        g_fontKaraokeMed = io.Fonts->AddFontFromFileTTF(bPath, 37.0f, nullptr, glyphRanges.Data);

        // Крупные шрифты (1440p / 2560x1440 и 4K Fullscreen)
        g_fontMainLarge = io.Fonts->AddFontFromFileTTF(fontPath, 22.0f, nullptr, glyphRanges.Data);
        g_fontTitleLarge = io.Fonts->AddFontFromFileTTF(bPath, 34.0f, nullptr, glyphRanges.Data);
        g_fontArtistLarge = io.Fonts->AddFontFromFileTTF(sbPath, 24.0f, nullptr, glyphRanges.Data);
        g_fontLyricsLarge = io.Fonts->AddFontFromFileTTF(fontPath, 35.0f, nullptr, glyphRanges.Data);
        g_fontKaraokeLarge = io.Fonts->AddFontFromFileTTF(bPath, 45.0f, nullptr, glyphRanges.Data);
    } else {
        g_fontMain = io.Fonts->AddFontDefault();
        g_fontMainBold = g_fontMain;
        g_fontTitleBold = g_fontMain;
        g_fontArtistMedium = g_fontMain;
        g_fontLyricsRegular = g_fontMain;
        g_fontKaraokeBold = g_fontMain;

        g_fontMainMed = g_fontMain;
        g_fontTitleMed = g_fontMain;
        g_fontArtistMed = g_fontMain;
        g_fontLyricsMed = g_fontMain;
        g_fontKaraokeMed = g_fontMain;

        g_fontMainLarge = g_fontMain;
        g_fontTitleLarge = g_fontMain;
        g_fontArtistLarge = g_fontMain;
        g_fontLyricsLarge = g_fontMain;
        g_fontKaraokeLarge = g_fontMain;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 22.0f;
    style.FrameRounding = 12.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 16.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowPadding = ImVec2(0, 0);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    AudioEngine audio;
    audio.init();

    Renderer renderer;
    renderer.init();

    queuePlaylistFromDisk();
    loadSettingsFromDisk(audio);
    renderer.setAAMode(g_savedAAMode);
    renderer.setReflectionsEnabled(g_savedReflections);

    // Инициализация аппаратных таймеров OpenGL для дебаг-монитора (0% CPU оверхед)
    if (GLAD_GL_VERSION_3_3 && glGenQueries != nullptr) {
        glGenQueries(kGpuQueryCount, g_gpuQueries);
        g_gpuQueriesInitialized = true;
    }

    // Инициализация счетчиков ЦПУ (под официальную формулу Windows Task Manager)
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    g_numLogicalCores = (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1;

    FILETIME ftCreation, ftExit, ftKernel, ftUser, ftNow;
    if (GetProcessTimes(GetCurrentProcess(), &ftCreation, &ftExit, &ftKernel, &ftUser)) {
        GetSystemTimeAsFileTime(&ftNow);
        g_prevProcTime = fileTimeToUint64(ftKernel) + fileTimeToUint64(ftUser);
        g_prevWallTime = fileTimeToUint64(ftNow);
    }
    g_lastTelemetrySampleTime = glfwGetTime();

    float discFlightProgress = 0.0f;
    float hudAlpha = 0.0f;
    int hoverIndex = -1;
    int cachedPickIndex = -1;
    double lastMouseX = -1.0, lastMouseY = -1.0;
    double lastActiveTime = glfwGetTime();
    double lastTime = glfwGetTime();

    glm::vec3 currentThemeAccent(0.95f, 0.26f, 0.42f);

    while (!glfwWindowShouldClose(window)) {
        double frameStart = glfwGetTime();

        bool isMinimized = IsIconic(hwnd) || (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0);
        if (isMinimized) {
            // Окно свёрнуто: ожидаем события SMTC / клики по кнопкам панели задач или таймаут 80 мс.
            // При клике в шторке Windows вызов PostMessage(hwnd, WM_NULL) мгновенно будит поток (0 мс задержки).
            glfwWaitEventsTimeout(0.08);
            double now = glfwGetTime();
            lastTime = now;

            // 1. Приём обложек из сети
            auto readyCovers = g_coverFetcher.pollCompleted();
            for (const auto& item : readyCovers) {
                for (auto& tr : g_playlist) {
                    if (tr.widePath == item.widePath && !tr.hasRealCover) {
                        glm::vec3 newAccent = tr.accentColor;
                        GLuint newTex = MetadataParser::uploadTextureFromMemory(item.imageData.data(), static_cast<int>(item.imageData.size()), &newAccent);
                        if (newTex != 0) {
                            if (tr.coverTex) glDeleteTextures(1, &tr.coverTex);
                            if (tr.badgeTex) glDeleteTextures(1, &tr.badgeTex);
                            tr.coverTex = newTex;
                            tr.hasRealCover = true;
                            tr.accentColor = newAccent;
                            tr.coverImagePath = CoverFetcher::getCachedCoverPath(tr.artist, tr.title).wstring();
                            tr.badgeTex = MetadataParser::createCoverBottomBadgeGDI(
                                MetadataParser::utf8ToWide(tr.artist),
                                MetadataParser::utf8ToWide(tr.title),
                                tr.accentColor
                            );
                            if (g_activePlayingTrack >= 0 && g_activePlayingTrack < static_cast<int>(g_playlist.size()) && &tr == &g_playlist[g_activePlayingTrack]) {
                                SmtcManager::instance().updateTrack(tr.title, tr.artist, tr.album, tr.coverImagePath);
                            }
                            lastActiveTime = now;
                        }
                        break;
                    }
                }
            }

            // 2. Очередь загрузки треков в фоне
            if (!g_pendingLoadQueue.empty()) {
                lastActiveTime = now;
                for (int k = 0; k < 2 && !g_pendingLoadQueue.empty(); ++k) {
                    std::wstring nextPath = g_pendingLoadQueue.front();
                    g_pendingLoadQueue.pop_front();
                    addTrackImmediate(nextPath);
                }
                if (g_pendingLoadQueue.empty() && g_pendingRestoreTrack >= 0 &&
                    g_pendingRestoreTrack < static_cast<int>(g_playlist.size()) && g_activePlayingTrack == -1) {
                    playTrackAndFetchLyrics(g_pendingRestoreTrack, audio, discFlightProgress, true);
                    g_pendingRestoreTrack = -1;
                }
            }

            // 2.5. Приём завершённых онлайн-скачиваний из Яндекс Музыки
            auto completedYmMin = YandexMusicManager::instance().pollCompletedDownloads();
            for (const auto& dl : completedYmMin) {
                bool added = addTrackImmediate(dl.localPath);
                int newIdx = -1;
                for (size_t i = 0; i < g_playlist.size(); ++i) {
                    if (g_playlist[i].widePath == dl.localPath) {
                        newIdx = static_cast<int>(i);
                        break;
                    }
                }
                if (newIdx >= 0) {
                    bool needRebadge = false;
                    if (!dl.title.empty() && (g_playlist[newIdx].title.empty() || g_playlist[newIdx].title.rfind("ym_", 0) == 0)) {
                        g_playlist[newIdx].title = dl.title;
                        needRebadge = true;
                    }
                    if (!dl.artist.empty() && (g_playlist[newIdx].artist == "Unknown Artist" || g_playlist[newIdx].artist.empty())) {
                        g_playlist[newIdx].artist = dl.artist;
                        needRebadge = true;
                    }
                    if (!dl.album.empty() && g_playlist[newIdx].album.empty()) {
                        g_playlist[newIdx].album = dl.album;
                        needRebadge = true;
                    }
                    if (!dl.coverPath.empty() && fs::exists(dl.coverPath)) {
                        if (g_playlist[newIdx].coverTex == 0 || !g_playlist[newIdx].hasRealCover) {
                            std::ifstream cf(dl.coverPath, std::ios::binary | std::ios::ate);
                            if (cf.is_open()) {
                                auto csz = cf.tellg();
                                if (csz > 300 && csz < 20 * 1024 * 1024) {
                                    cf.seekg(0, std::ios::beg);
                                    std::vector<uint8_t> cbuf(static_cast<size_t>(csz));
                                    if (cf.read(reinterpret_cast<char*>(cbuf.data()), csz)) {
                                        glm::vec3 acc = g_playlist[newIdx].accentColor;
                                        GLuint ctex = MetadataParser::uploadTextureFromMemory(cbuf.data(), static_cast<int>(cbuf.size()), &acc);
                                        if (ctex != 0) {
                                            if (g_playlist[newIdx].coverTex) glDeleteTextures(1, &g_playlist[newIdx].coverTex);
                                            g_playlist[newIdx].coverTex = ctex;
                                            g_playlist[newIdx].hasRealCover = true;
                                            g_playlist[newIdx].accentColor = acc;
                                            g_playlist[newIdx].coverImagePath = dl.coverPath;
                                            needRebadge = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (needRebadge) {
                        std::wstring wArtist = MetadataParser::utf8ToWide(g_playlist[newIdx].artist);
                        std::wstring wTitle = MetadataParser::utf8ToWide(g_playlist[newIdx].title);
                        std::wstring wAlbum = MetadataParser::utf8ToWide(g_playlist[newIdx].album);
                        g_playlist[newIdx].searchKeyLower = MetadataParser::toLowerWide(wTitle + L" " + wArtist + L" " + wAlbum);
                        if (g_playlist[newIdx].badgeTex != 0) {
                            glDeleteTextures(1, &g_playlist[newIdx].badgeTex);
                        }
                        g_playlist[newIdx].badgeTex = MetadataParser::createCoverBottomBadgeGDI(wArtist, wTitle, g_playlist[newIdx].accentColor);
                    }
                    g_searchDirty = true;
                    rebuildVisibleIndicesIfNeeded();
                    playTrackAndFetchLyrics(newIdx, audio, discFlightProgress);
                    focusTrackOnShelf(newIdx, g_visibleIndices);
                }
                if (added) {
                    savePlaylistToDisk();
                }
                lastActiveTime = now;
            }

            rebuildVisibleIndicesIfNeeded();

            // 3. Обработка команд SMTC (Play, Pause, Next, Prev, Stop)
            processMediaActions(audio, discFlightProgress, now, lastActiveTime);

            // 4. Автоматическое переключение на следующий трек при завершении
            checkTrackEndAndAdvance(audio, discFlightProgress, now, lastActiveTime);

            // 5. Синхронизация статуса воспроизведения с SMTC и миниатюрой
            syncSmtcPlaybackState(audio);

            continue;
        }

        glfwPollEvents();

        double now = glfwGetTime();
        float dt = std::clamp(static_cast<float>(now - lastTime), 0.0001f, 0.05f);
        lastTime = now;

        // Мгновенная обработка команд SMTC и панели задач в активном режиме
        processMediaActions(audio, discFlightProgress, now, lastActiveTime);

        // Приём завершённых онлайн-скачиваний из Яндекс Музыки
        auto completedYm = YandexMusicManager::instance().pollCompletedDownloads();
        for (const auto& dl : completedYm) {
            // Мгновенный фоновый предзапрос караоке текста с известной длительностью трека
            if (!dl.localPath.empty() && !dl.title.empty()) {
                g_lyricsFetcher.requestLyrics(dl.localPath, dl.title, dl.artist, dl.duration);
            }
            bool added = addTrackImmediate(dl.localPath);
            int newIdx = -1;
            for (size_t i = 0; i < g_playlist.size(); ++i) {
                if (g_playlist[i].widePath == dl.localPath) {
                    newIdx = static_cast<int>(i);
                    break;
                }
            }
            if (newIdx >= 0) {
                bool needRebadge = false;
                if (!dl.title.empty() && (g_playlist[newIdx].title.empty() || g_playlist[newIdx].title.rfind("ym_", 0) == 0)) {
                    g_playlist[newIdx].title = dl.title;
                    needRebadge = true;
                }
                if (!dl.artist.empty() && (g_playlist[newIdx].artist == "Unknown Artist" || g_playlist[newIdx].artist.empty())) {
                    g_playlist[newIdx].artist = dl.artist;
                    needRebadge = true;
                }
                if (!dl.album.empty() && g_playlist[newIdx].album.empty()) {
                    g_playlist[newIdx].album = dl.album;
                    needRebadge = true;
                }
                if (!dl.coverPath.empty() && fs::exists(dl.coverPath)) {
                    if (g_playlist[newIdx].coverTex == 0 || !g_playlist[newIdx].hasRealCover) {
                        std::ifstream cf(dl.coverPath, std::ios::binary | std::ios::ate);
                        if (cf.is_open()) {
                            auto csz = cf.tellg();
                            if (csz > 300 && csz < 20 * 1024 * 1024) {
                                cf.seekg(0, std::ios::beg);
                                std::vector<uint8_t> cbuf(static_cast<size_t>(csz));
                                if (cf.read(reinterpret_cast<char*>(cbuf.data()), csz)) {
                                    glm::vec3 acc = g_playlist[newIdx].accentColor;
                                    GLuint ctex = MetadataParser::uploadTextureFromMemory(cbuf.data(), static_cast<int>(cbuf.size()), &acc);
                                    if (ctex != 0) {
                                        if (g_playlist[newIdx].coverTex) glDeleteTextures(1, &g_playlist[newIdx].coverTex);
                                        g_playlist[newIdx].coverTex = ctex;
                                        g_playlist[newIdx].hasRealCover = true;
                                        g_playlist[newIdx].accentColor = acc;
                                        g_playlist[newIdx].coverImagePath = dl.coverPath;
                                        needRebadge = true;
                                    }
                                }
                            }
                        }
                    }
                }
                if (needRebadge) {
                    std::wstring wArtist = MetadataParser::utf8ToWide(g_playlist[newIdx].artist);
                    std::wstring wTitle = MetadataParser::utf8ToWide(g_playlist[newIdx].title);
                    std::wstring wAlbum = MetadataParser::utf8ToWide(g_playlist[newIdx].album);
                    g_playlist[newIdx].searchKeyLower = MetadataParser::toLowerWide(wTitle + L" " + wArtist + L" " + wAlbum);
                    if (g_playlist[newIdx].badgeTex != 0) {
                        glDeleteTextures(1, &g_playlist[newIdx].badgeTex);
                    }
                    g_playlist[newIdx].badgeTex = MetadataParser::createCoverBottomBadgeGDI(wArtist, wTitle, g_playlist[newIdx].accentColor);
                }
                g_searchDirty = true;
                rebuildVisibleIndicesIfNeeded();
                playTrackAndFetchLyrics(newIdx, audio, discFlightProgress);
                focusTrackOnShelf(newIdx, g_visibleIndices);
            }
            if (added) {
                savePlaylistToDisk();
            }
            lastActiveTime = now;
        }

        // Асинхронный поиск в Яндекс Музыке с дебаунсом 280 мс (только в режиме Яндекс)
        if (g_searchSource == SearchSource::Yandex) {
            std::string curQuery = g_searchQuery;
            if (curQuery != g_ymLastSearchQuery) {
                g_ymLastSearchTypingTime = now;
                g_ymLastSearchQuery = curQuery;
            }
            if (curQuery.length() >= 2) {
                static std::string s_lastSearchedQuery;
                if (now - g_ymLastSearchTypingTime > 0.28 && s_lastSearchedQuery != curQuery) {
                    s_lastSearchedQuery = curQuery;
                    YandexMusicManager::instance().searchAsync(curQuery);
                }
            } else if (curQuery.empty()) {
                YandexMusicManager::instance().clearSearchResults();
            }
        }

        if (!g_pendingLoadQueue.empty()) {
            lastActiveTime = now;
            for (int k = 0; k < 2 && !g_pendingLoadQueue.empty(); ++k) {
                std::wstring nextPath = g_pendingLoadQueue.front();
                g_pendingLoadQueue.pop_front();
                addTrackImmediate(nextPath);
            }
            if (g_pendingLoadQueue.empty() && g_pendingRestoreTrack >= 0 &&
                g_pendingRestoreTrack < static_cast<int>(g_playlist.size()) && g_activePlayingTrack == -1) {
                playTrackAndFetchLyrics(g_pendingRestoreTrack, audio, discFlightProgress, true);
                g_pendingRestoreTrack = -1;
            }
        }

        // Приём завершённых фоновых загрузок обложек из сети
        auto readyCovers = g_coverFetcher.pollCompleted();
        for (const auto& item : readyCovers) {
            for (auto& tr : g_playlist) {
                if (tr.widePath == item.widePath && !tr.hasRealCover) {
                    glm::vec3 newAccent = tr.accentColor;
                    GLuint newTex = MetadataParser::uploadTextureFromMemory(item.imageData.data(), static_cast<int>(item.imageData.size()), &newAccent);
                    if (newTex != 0) {
                        if (tr.coverTex) glDeleteTextures(1, &tr.coverTex);
                        if (tr.badgeTex) glDeleteTextures(1, &tr.badgeTex);
                        tr.coverTex = newTex;
                        tr.hasRealCover = true;
                        tr.accentColor = newAccent;
                        tr.coverImagePath = CoverFetcher::getCachedCoverPath(tr.artist, tr.title).wstring();
                        tr.badgeTex = MetadataParser::createCoverBottomBadgeGDI(
                            MetadataParser::utf8ToWide(tr.artist),
                            MetadataParser::utf8ToWide(tr.title),
                            tr.accentColor
                        );
                        if (g_activePlayingTrack >= 0 && g_activePlayingTrack < static_cast<int>(g_playlist.size()) && &tr == &g_playlist[g_activePlayingTrack]) {
                            SmtcManager::instance().updateTrack(tr.title, tr.artist, tr.album, tr.coverImagePath);
                        }
                        lastActiveTime = now;
                    }
                    break;
                }
            }
        }

        int w = 1280, h = 720;
        glfwGetFramebufferSize(window, &w, &h);
        if (w <= 0 || h <= 0) continue;
        renderer.resize(w, h);

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        bool mouseMoved = (std::abs(mouseX - lastMouseX) > 0.25 || std::abs(mouseY - lastMouseY) > 0.25);
        if (mouseMoved || io.MouseDown[0] || io.MouseDown[1] || io.MouseWheel != 0.0f) {
            lastActiveTime = now;
        }
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        rebuildVisibleIndicesIfNeeded();

        // Плавная анимация 3D-разворота бокса (Lyrics / Караоке на задней стороне)
        float targetInspect = (g_lyricsInspectMode && !g_playlist.empty()) ? 1.0f : 0.0f;
        if (std::abs(targetInspect - g_inspectProgress) > 0.001f) {
            float speed = g_lyricsInspectMode ? 2.4f : 2.8f;
            if (g_inspectProgress < targetInspect) {
                g_inspectProgress = std::min(1.0f, g_inspectProgress + dt * speed);
            } else {
                g_inspectProgress = std::max(0.0f, g_inspectProgress - dt * speed);
            }
            lastActiveTime = now;
        } else {
            g_inspectProgress = targetInspect;
        }

        float targetFrontFlip = g_inspectShowFront ? 1.0f : 0.0f;
        if (std::abs(targetFrontFlip - g_inspectFrontFlip) > 0.001f) {
            if (g_inspectFrontFlip < targetFrontFlip) {
                g_inspectFrontFlip = std::min(1.0f, g_inspectFrontFlip + dt * 3.2f);
            } else {
                g_inspectFrontFlip = std::max(0.0f, g_inspectFrontFlip - dt * 3.2f);
            }
            lastActiveTime = now;
        } else {
            g_inspectFrontFlip = targetFrontFlip;
        }

        if (g_roulette.active) {
            lastActiveTime = now;
            if (g_visibleIndices.empty()) {
                g_roulette.active = false;
                g_roulette.currentTrack = -1;
            } else {
                g_roulette.stepTimer += dt;
                if (g_roulette.stepTimer >= g_roulette.stepDuration) {
                    g_roulette.stepTimer = 0.0f;
                    g_roulette.stepsRemaining--;

                    if (g_roulette.stepsRemaining <= 0) {
                        g_roulette.active = false;
                        int winSlot = std::clamp(g_roulette.targetSlot, 0, static_cast<int>(g_visibleIndices.size()) - 1);
                        int winTrack = g_visibleIndices[winSlot];
                        g_roulette.currentSlot = winSlot;
                        g_roulette.currentTrack = winTrack;

                        if (winTrack >= 0 && winTrack < static_cast<int>(g_playlist.size())) {
                            playTrackAndFetchLyrics(winTrack, audio, discFlightProgress);
                        }
                    } else {
                        if (g_roulette.stepsRemaining == 1) {
                            g_roulette.currentSlot = std::clamp(g_roulette.targetSlot, 0, static_cast<int>(g_visibleIndices.size()) - 1);
                        } else {
                            int lo = std::clamp(g_roulette.winSlotStart, 0, static_cast<int>(g_visibleIndices.size()) - 1);
                            int hi = std::clamp(g_roulette.winSlotEnd, lo, static_cast<int>(g_visibleIndices.size()) - 1);
                            std::uniform_int_distribution<int> localDist(lo, hi);
                            int nextSlot = localDist(getRng());
                            if (nextSlot == g_roulette.currentSlot && hi > lo) {
                                nextSlot = lo + (nextSlot + 1 - lo) % (hi - lo + 1);
                            }
                            g_roulette.currentSlot = nextSlot;
                        }
                        g_roulette.currentTrack = g_visibleIndices[g_roulette.currentSlot];

                        float progress = 1.0f - static_cast<float>(g_roulette.stepsRemaining) / static_cast<float>(g_roulette.totalSteps);
                        g_roulette.stepDuration = 0.055f + std::pow(progress, 2.3f) * 0.26f;
                    }
                }
            }
        }

        // Скролл полки мышью и плавный автоскролл у краёв (отключён, когда открыт режим осмотра задней стороны бокса)
        if (!io.WantCaptureMouse && !g_visibleIndices.empty() && !g_roulette.active && g_inspectProgress < 0.1f) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                g_targetScroll -= io.MouseDelta.x * 0.0045f;
                lastActiveTime = now;
            } else if (mouseY > 85 && mouseY < h - 115) {
                float edgeZone = 85.0f;
                if (mouseX < edgeZone && mouseX >= 0) {
                    float factor = (edgeZone - static_cast<float>(mouseX)) / edgeZone;
                    g_targetScroll -= factor * 1.35f * dt;
                    lastActiveTime = now;
                } else if (mouseX > w - edgeZone && mouseX <= w) {
                    float factor = (static_cast<float>(mouseX) - (w - edgeZone)) / edgeZone;
                    g_targetScroll += factor * 1.35f * dt;
                    lastActiveTime = now;
                }
            }
        }

        bool isScrolling = false;
        if (!g_visibleIndices.empty()) {
            float maxScroll = (std::max)(0.0f, (static_cast<float>(g_visibleIndices.size()) - 1.0f) * kSpacing);
            const float overscroll = kSpacing * 1.5f;
            g_targetScroll = std::clamp(g_targetScroll, -overscroll, maxScroll + overscroll);

            // Мягкий возврат (rubber band / spring back) при выходе за пределы крайних обложек
            if (g_targetScroll < 0.0f) {
                float returnAlpha = 1.0f - std::exp(-10.0f * dt);
                g_targetScroll += (0.0f - g_targetScroll) * returnAlpha;
            } else if (g_targetScroll > maxScroll) {
                float returnAlpha = 1.0f - std::exp(-10.0f * dt);
                g_targetScroll += (maxScroll - g_targetScroll) * returnAlpha;
            }

            float diffScroll = g_targetScroll - g_shelfScroll;
            if (std::abs(diffScroll) > 0.0001f) {
                // Высокоточная экспоненциальная интерполяция: абсолютно бархатный ход полки при любой скорости вращения колеса,
                // нулевой рывок (zero jerk), отсутствие ступенчатости и полная независимость от частоты кадров
                float alpha = 1.0f - std::exp(-13.5f * dt);
                g_shelfScroll += diffScroll * alpha;
                isScrolling = true;
                lastActiveTime = now;
            } else {
                g_shelfScroll = g_targetScroll;
            }
        } else {
            g_targetScroll = 0.0f;
            g_shelfScroll = 0.0f;
        }

        if (g_roulette.active) {
            hoverIndex = g_roulette.currentTrack;
        } else if (io.WantCaptureMouse || g_visibleIndices.empty() || mouseY < 75.0 || g_inspectProgress > 0.1f) {
            hoverIndex = -1;
            cachedPickIndex = -1;
        } else if (mouseMoved || isScrolling || !g_pendingLoadQueue.empty()) {
            // Во время пролистывания полки обложки непрерывно реагируют на курсор:
            // Луч проверяется каждый кадр движения, обеспечивая живой тактильный отклик
            cachedPickIndex = renderer.pickTrack(mouseX, mouseY, g_visibleIndices, g_hoverWeights, g_shelfScroll);
            hoverIndex = cachedPickIndex;
        } else {
            hoverIndex = cachedPickIndex;
        }

        for (size_t i = 0; i < g_hoverWeights.size(); ++i) {
            float targetW = (static_cast<int>(i) == hoverIndex) ? 1.0f : 0.0f;
            if (g_hoverWeights[i] == targetW) continue;
            float diffW = targetW - g_hoverWeights[i];
            if (std::abs(diffW) > 0.001f) {
                float speed = g_roulette.active ? 18.0f : 14.0f;
                float alpha = 1.0f - std::exp(-speed * dt);
                g_hoverWeights[i] += diffW * alpha;
                lastActiveTime = now;
            } else {
                g_hoverWeights[i] = targetW;
            }
        }

        if (g_selectedTrack != -1) {
            if (discFlightProgress < 1.0f) {
                discFlightProgress = std::min(1.0f, discFlightProgress + dt * 1.95f);
                lastActiveTime = now;
            }
        } else {
            discFlightProgress = 0.0f;
        }

        checkTrackEndAndAdvance(audio, discFlightProgress, now, lastActiveTime);

        int accentSourceIdx = g_roulette.active ? g_roulette.currentTrack : g_activePlayingTrack;
        glm::vec3 targetAccent = (accentSourceIdx >= 0 && accentSourceIdx < static_cast<int>(g_playlist.size()))
            ? g_playlist[accentSourceIdx].accentColor
            : glm::vec3(0.95f, 0.26f, 0.42f);
        currentThemeAccent = renderer.getCurrentAccent();

        float targetHud = (!g_playlist.empty()) ? 1.0f : 0.0f;
        hudAlpha += (targetHud - hudAlpha) * std::min(1.0f, 9.0f * dt);

        // ------------------ ЗАМЕР GPU И СБОР ТЕЛЕМЕТРИИ ------------------
        if (g_gpuQueriesInitialized) {
            // Собираем завершённые замеры GPU из кольцевого буфера без ожидания процессора
            for (int i = 0; i < kGpuQueryCount; ++i) {
                if (g_gpuQueryInFlight[i] && i != g_gpuQueryWriteIdx) {
                    GLint available = 0;
                    glGetQueryObjectiv(g_gpuQueries[i], GL_QUERY_RESULT_AVAILABLE, &available);
                    if (available) {
                        GLuint64 elapsedNs = 0;
                        glGetQueryObjectui64v(g_gpuQueries[i], GL_QUERY_RESULT, &elapsedNs);
                        g_accumGpuTimeNs += elapsedNs;
                        g_gpuQueryInFlight[i] = false;
                    }
                }
            }
        }

        bool gpuQueryActive = false;
        if (g_gpuQueriesInitialized) {
            glBeginQuery(GL_TIME_ELAPSED, g_gpuQueries[g_gpuQueryWriteIdx]);
            g_gpuQueryInFlight[g_gpuQueryWriteIdx] = true;
            gpuQueryActive = true;
        }

        int rouletteHi = g_roulette.active ? g_roulette.currentTrack : -1;
        renderer.renderShelf(g_playlist, g_visibleIndices, g_hoverWeights, g_shelfScroll,
                             g_selectedTrack, g_activePlayingTrack, rouletteHi,
                             discFlightProgress, g_inspectProgress, g_inspectFrontFlip,
                             static_cast<float>(now), targetAccent);

        io.FontDefault = (g_boldInterfaceFont && g_fontMainBold) ? g_fontMainBold : g_fontMain;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Синхронизация состояния воспроизведения с SMTC и кнопками миниатюры панели задач
        syncSmtcPlaybackState(audio);

        // Переключение дебаг-монитора по горячей клавише F3 (доступно в любой момент)
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
            g_showDebugOverlay = !g_showDebugOverlay;
            saveSettingsToDisk(audio);
            lastActiveTime = now;
        }

        int displayTrackIdx = (g_activePlayingTrack >= 0 && g_activePlayingTrack < static_cast<int>(g_playlist.size()))
            ? g_activePlayingTrack
            : 0;

        if (!io.WantTextInput) {
            bool ctrl = io.KeyCtrl;
            bool shift = io.KeyShift;

            // Воспроизведение / Пауза: Space
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                if (g_activePlayingTrack != -1) {
                    audio.togglePlayPause();
                } else if (!g_visibleIndices.empty()) {
                    playTrackAndFetchLyrics(g_visibleIndices[0], audio, discFlightProgress);
                }
                lastActiveTime = now;
            }

            // Следующий трек: Ctrl + Right, N, Period (>)
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ||
                ImGui::IsKeyPressed(ImGuiKey_N) ||
                (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Period))) {
                const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                int nextIdx = getNextTrackIndex(listToUse, true);
                if (nextIdx >= 0) {
                    playTrackAndFetchLyrics(nextIdx, audio, discFlightProgress);
                    focusTrackOnShelf(nextIdx, g_visibleIndices);
                }
                lastActiveTime = now;
            }

            // Предыдущий трек: Ctrl + Left, P, Comma (<)
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ||
                ImGui::IsKeyPressed(ImGuiKey_P) ||
                (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Comma))) {
                if (audio.getPlaybackPosition() > 3.0f) {
                    audio.seekTo(0.0f);
                } else {
                    const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                    int prevIdx = getPrevTrackIndex(listToUse);
                    if (prevIdx >= 0) {
                        playTrackAndFetchLyrics(prevIdx, audio, discFlightProgress);
                        focusTrackOnShelf(prevIdx, g_visibleIndices);
                    }
                }
                lastActiveTime = now;
            }

            // Перемотка на ±5 сек (с Shift: ±15 сек)
            if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                audio.seekBySeconds(shift ? -15.0f : -5.0f);
                lastActiveTime = now;
            }
            if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                audio.seekBySeconds(shift ? 15.0f : 5.0f);
                lastActiveTime = now;
            }

            // Громкость: Up / Down
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                audio.setVolume(audio.getVolume() + 0.05f);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                audio.setVolume(audio.getVolume() - 0.05f);
                lastActiveTime = now;
            }

            // Заглушить звук (Mute): M
            if (ImGui::IsKeyPressed(ImGuiKey_M)) {
                audio.toggleMute();
                lastActiveTime = now;
            }

            // Случайный порядок (Shuffle): S
            if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                g_isShuffle = !g_isShuffle;
                saveSettingsToDisk(audio);
                lastActiveTime = now;
            }

            // Режим повтора (Repeat/Loop): Ctrl+R или T
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_R)) || ImGui::IsKeyPressed(ImGuiKey_T)) {
                LoopMode lm = audio.getLoopMode();
                if (lm == LoopMode::All) audio.setLoopMode(LoopMode::Track);
                else if (lm == LoopMode::Track) audio.setLoopMode(LoopMode::None);
                else audio.setLoopMode(LoopMode::All);
                saveSettingsToDisk(audio);
                lastActiveTime = now;
            }

            // Текст песни / Оборот бокса: L
            if (ImGui::IsKeyPressed(ImGuiKey_L) && !g_playlist.empty()) {
                g_lyricsInspectMode = !g_lyricsInspectMode;
                g_inspectShowFront = false;
                if (g_lyricsInspectMode) {
                    triggerAutoLyricsForTrack(displayTrackIdx, audio);
                }
                lastActiveTime = now;
            }

            // Рулетка случайного выбора: R (без Ctrl)
            if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_R) && !g_visibleIndices.empty()) {
                startRandomRoulette(g_visibleIndices, audio, discFlightProgress);
                lastActiveTime = now;
            }

            // Добавить файлы (Ctrl+O) / папку (Ctrl+Shift+O)
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
                if (shift) {
                    openFolderDialog(hwnd);
                } else {
                    openFileDialog(hwnd);
                }
                lastActiveTime = now;
            }

            // Быстрый поиск: Ctrl+F или Slash (/)
            if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) || ImGui::IsKeyPressed(ImGuiKey_Slash)) {
                g_focusSearchInput = true;
                lastActiveTime = now;
            }

            // Закрыть / сброс: Escape
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (g_showSettingsWindow) {
                    g_showSettingsWindow = false;
                } else if (g_lyricsInspectMode) {
                    g_lyricsInspectMode = false;
                } else if (g_searchQuery[0] != '\0') {
                    g_searchQuery[0] = '\0';
                    g_searchDirty = true;
                    YandexMusicManager::instance().clearSearchResults();
                }
                lastActiveTime = now;
            }
        }

        // ================== 1. ЗАДНЯЯ СТОРОНА 3D-БОКСА: АВТОПОИСК ТЕКСТА И РЕЖИМ КАРАОКЕ ==================
        if (g_inspectProgress > 0.40f && !g_playlist.empty()) {
            const auto& curTrack = g_playlist[displayTrackIdx];
            triggerAutoLyricsForTrack(displayTrackIdx, audio);

            glm::vec4 boxRect = renderer.getInspectBoxScreenRect();
            float boxW = boxRect.z - boxRect.x;
            float boxH = boxRect.w - boxRect.y;

            // Плавное проявление контента задней стороны по мере разворота бокса
            float backAlpha = std::clamp((g_inspectProgress - 0.40f) / 0.60f, 0.0f, 1.0f);

            if (backAlpha > 0.02f && boxW > 120.0f && boxH > 120.0f) {
                LyricsResult lyr = g_lyricsFetcher.getLyrics(curTrack.widePath);

                // Определение масштаба: 0 = стандарт (оконный ~800p), 1 = средний (1080p), 2 = крупный (1440p QHD / 4K)
                int scaleTier = 0;
                if (boxH >= 860.0f) {
                    scaleTier = 2;
                } else if (boxH >= 650.0f) {
                    scaleTier = 1;
                }

                ImFont* fontBase = (scaleTier == 2 && g_fontMainLarge) ? g_fontMainLarge :
                                   (scaleTier == 1 && g_fontMainMed) ? g_fontMainMed : g_fontMain;
                ImFont* fontTitle = (scaleTier == 2 && g_fontTitleLarge) ? g_fontTitleLarge :
                                    (scaleTier == 1 && g_fontTitleMed) ? g_fontTitleMed : g_fontTitleBold;
                ImFont* fontArtist = (scaleTier == 2 && g_fontArtistLarge) ? g_fontArtistLarge :
                                     (scaleTier == 1 && g_fontArtistMed) ? g_fontArtistMed : g_fontArtistMedium;
                ImFont* fontLyrics = (scaleTier == 2 && g_fontLyricsLarge) ? g_fontLyricsLarge :
                                     (scaleTier == 1 && g_fontLyricsMed) ? g_fontLyricsMed : g_fontLyricsRegular;
                ImFont* fontKaraoke = (scaleTier == 2 && g_fontKaraokeLarge) ? g_fontKaraokeLarge :
                                      (scaleTier == 1 && g_fontKaraokeMed) ? g_fontKaraokeMed : g_fontKaraokeBold;

                ImGui::SetNextWindowPos(ImVec2(boxRect.x, boxRect.y), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(boxW, boxH), ImGuiCond_Always);

                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // Прозрачный — лежит прямо на 3D-грани бокса!
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.45f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.75f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.95f * backAlpha));

                ImGui::Begin("##BackOfBoxLyricsSurface", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
                if (fontBase) ImGui::PushFont(fontBase);

                ImDrawList* bdl = ImGui::GetWindowDrawList();
                ImVec2 wp = ImGui::GetWindowPos();

                float hdrGradH = (scaleTier == 2) ? 96.0f : ((scaleTier == 1) ? 82.0f : 68.0f);
                float padX = (scaleTier == 2) ? 22.0f : ((scaleTier == 1) ? 18.0f : 16.0f);

                // Полупрозрачная затемнённая подложка-градиент под шапкой
                bdl->AddRectFilledMultiColor(
                    ImVec2(wp.x, wp.y),
                    ImVec2(wp.x + boxW, wp.y + hdrGradH),
                    ImColor(0.02f, 0.03f, 0.05f, 0.65f * backAlpha),
                    ImColor(0.02f, 0.03f, 0.05f, 0.65f * backAlpha),
                    ImColor(0.0f, 0.0f, 0.0f, 0.0f),
                    ImColor(0.0f, 0.0f, 0.0f, 0.0f)
                );
                // Тонкая акцентная линия-разделитель под шапкой
                bdl->AddLine(
                    ImVec2(wp.x + padX, wp.y + hdrGradH),
                    ImVec2(wp.x + boxW - padX, wp.y + hdrGradH),
                    ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.28f * backAlpha),
                    1.0f
                );

                // Верхняя шапка заднего вкладыша: номер трека, название, артист
                char trkNumBuf[32];
                std::snprintf(trkNumBuf, sizeof(trkNumBuf), "TRACK %02d", displayTrackIdx + 1);
                float trkNumY = (scaleTier == 2) ? 9.0f : ((scaleTier == 1) ? 8.0f : 7.0f);
                bdl->AddText(
                    ImVec2(wp.x + padX + 2.0f, wp.y + trkNumY),
                    ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.90f * backAlpha),
                    trkNumBuf
                );

                int maxTitleLen = (boxW > 950.0f) ? 48 : ((boxW > 680.0f) ? 36 : 24);
                int maxArtistLen = (boxW > 950.0f) ? 54 : ((boxW > 680.0f) ? 40 : 28);
                std::string hdrTitle = truncateUtf8(curTrack.title, maxTitleLen);
                std::string hdrArtist = truncateUtf8(curTrack.artist, maxArtistLen);

                // Кнопка закрытия [X] в правом верхнем углу прямо на карте
                float btnSz = (scaleTier == 2) ? 30.0f : ((scaleTier == 1) ? 26.0f : 24.0f);
                float closeBtnX = boxW - btnSz - (scaleTier == 2 ? 16.0f : 12.0f);
                float btnY = (scaleTier == 2) ? 12.0f : ((scaleTier == 1) ? 11.0f : 10.0f);
                ImGui::SetCursorPos(ImVec2(closeBtnX, btnY));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.22f, 0.28f, 0.85f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.15f, 0.20f, 0.95f * backAlpha));
                if (ImGui::Button("X##CloseInspectBox", ImVec2(btnSz, btnSz))) {
                    g_lyricsInspectMode = false;
                }
                ImGui::PopStyleColor(3);

                // Кнопка повторного поиска текста [R]
                float retryBtnX = closeBtnX - btnSz - 6.0f;
                ImGui::SetCursorPos(ImVec2(retryBtnX, btnY));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.07f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.70f * backAlpha));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.90f * backAlpha));
                if (ImGui::Button("R##RetryLyrics", ImVec2(btnSz, btnSz))) {
                    int durSec = static_cast<int>(std::round(audio.getDuration()));
                    g_lyricsFetcher.forceRefresh(curTrack.widePath, curTrack.title, curTrack.artist, durSec, curTrack.embeddedLyrics);
                }
                ImGui::PopStyleColor(3);

                // Бейдж режима (KARAOKE • LRCLIB / TEXT • GENIUS)
                std::string badgeStr = lyr.sourceBadge.empty() ? "LYRICS" : lyr.sourceBadge;
                ImVec2 bSz = ImGui::CalcTextSize(badgeStr.c_str());
                float pillW = bSz.x + (scaleTier == 2 ? 28.0f : 22.0f);
                float pillH = (scaleTier == 2) ? 28.0f : ((scaleTier == 1) ? 24.0f : 22.0f);
                float pillX = retryBtnX - pillW - (scaleTier == 2 ? 10.0f : 8.0f);
                float pillY = btnY + (btnSz - pillH) * 0.5f;

                ImVec4 pillBg = (lyr.status == LyricsStatus::FoundSynced)
                    ? ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha)
                    : ImVec4(1.0f, 1.0f, 1.0f, 0.08f * backAlpha);
                bdl->AddRectFilled(ImVec2(wp.x + pillX, wp.y + pillY),
                                   ImVec2(wp.x + pillX + pillW, wp.y + pillY + pillH),
                                   ImColor(pillBg), pillH * 0.5f);
                bdl->AddRect(ImVec2(wp.x + pillX, wp.y + pillY),
                             ImVec2(wp.x + pillX + pillW, wp.y + pillY + pillH),
                             ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.55f * backAlpha), pillH * 0.5f);

                // Пульсирующая точка синхронизации караоке
                if (lyr.status == LyricsStatus::FoundSynced) {
                    float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 5.0f);
                    float dotRadius = (scaleTier == 2) ? 3.5f : 3.0f;
                    float dotX = wp.x + pillX + (scaleTier == 2 ? 12.0f : 9.0f);
                    bdl->AddCircleFilled(
                        ImVec2(dotX, wp.y + pillY + pillH * 0.5f),
                        dotRadius,
                        ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, (0.6f + 0.4f * pulse) * backAlpha)
                    );
                }

                ImGui::SetCursorPos(ImVec2(pillX + (lyr.status == LyricsStatus::FoundSynced ? (scaleTier == 2 ? 20.0f : 16.0f) : 10.0f), pillY + (pillH - bSz.y) * 0.5f));
                ImGui::TextColored(ImVec4(0.96f, 0.98f, 1.0f, backAlpha), "%s", badgeStr.c_str());

                // Заголовок трека с тенью
                float titleY = (scaleTier == 2) ? 28.0f : ((scaleTier == 1) ? 24.0f : 21.0f);
                float artistY = (scaleTier == 2) ? 66.0f : ((scaleTier == 1) ? 54.0f : 45.0f);
                float titleX = padX + 2.0f;

                if (fontTitle) ImGui::PushFont(fontTitle);
                ImGui::SetCursorPos(ImVec2(titleX + 1.0f, titleY + 1.0f));
                ImGui::TextColored(ImVec4(0.01f, 0.02f, 0.04f, 0.70f * backAlpha), "%s", hdrTitle.c_str());
                ImGui::SetCursorPos(ImVec2(titleX, titleY));
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, backAlpha), "%s", hdrTitle.c_str());
                if (fontTitle) ImGui::PopFont();

                // Исполнитель
                if (fontArtist) ImGui::PushFont(fontArtist);
                ImGui::SetCursorPos(ImVec2(titleX + 1.0f, artistY + 1.0f));
                ImGui::TextColored(ImVec4(0.01f, 0.02f, 0.04f, 0.50f * backAlpha), "%s", hdrArtist.c_str());
                ImGui::SetCursorPos(ImVec2(titleX, artistY));
                ImGui::TextColored(ImVec4(0.78f, 0.83f, 0.92f, 0.95f * backAlpha), "%s", hdrArtist.c_str());
                if (fontArtist) ImGui::PopFont();

                // Область прокрутки текста на задней крышке бокса
                float contentTop = (scaleTier == 2) ? 104.0f : ((scaleTier == 1) ? 88.0f : 72.0f);
                float footerH = (scaleTier == 2) ? 44.0f : ((scaleTier == 1) ? 38.0f : 34.0f);
                float contentH = boxH - contentTop - footerH;

                // Аккуратная полупрозрачная подложка под текст для идеальной читаемости
                float platePadX = (scaleTier == 2) ? 20.0f : ((scaleTier == 1) ? 16.0f : 14.0f);
                float plateTop = contentTop - 4.0f;
                float plateBottom = boxH - footerH + 4.0f;
                ImVec2 plateMin(wp.x + platePadX, wp.y + plateTop);
                ImVec2 plateMax(wp.x + boxW - platePadX, wp.y + plateBottom);
                float plateRounding = (scaleTier == 2) ? 18.0f : 14.0f;

                const float plateOpacity = 0.30f;

                // 1. Мягкая объёмная тень подложки
                bdl->AddRectFilled(
                    ImVec2(plateMin.x - 2.0f, plateMin.y),
                    ImVec2(plateMax.x + 2.0f, plateMax.y + 4.0f),
                    ImColor(0.0f, 0.0f, 0.0f, (plateOpacity * 0.60f) * backAlpha),
                    plateRounding + 2.0f
                );

                // 2. Полупрозрачная тонированная основа (Dark Frosted Glass)
                bdl->AddRectFilled(
                    plateMin,
                    plateMax,
                    ImColor(0.035f, 0.042f, 0.058f, plateOpacity * backAlpha),
                    plateRounding
                );

                // 3. Вертикальный градиент глубины (чуть светлее сверху, глубже снизу)
                bdl->AddRectFilledMultiColor(
                    plateMin,
                    plateMax,
                    ImColor(0.04f, 0.05f, 0.07f, (plateOpacity * 0.40f) * backAlpha),
                    ImColor(0.04f, 0.05f, 0.07f, (plateOpacity * 0.40f) * backAlpha),
                    ImColor(0.015f, 0.02f, 0.03f, (plateOpacity * 0.80f) * backAlpha),
                    ImColor(0.015f, 0.02f, 0.03f, (plateOpacity * 0.80f) * backAlpha)
                );

                // 4. Тонкая аккуратная рамка по периметру
                bdl->AddRect(
                    plateMin,
                    plateMax,
                    ImColor(1.0f, 1.0f, 1.0f, 0.09f * backAlpha),
                    plateRounding,
                    0,
                    1.0f
                );

                // 5. Деликатный световой блик на верхней кромке подложки
                bdl->AddLine(
                    ImVec2(plateMin.x + plateRounding, plateMin.y + 0.5f),
                    ImVec2(plateMax.x - plateRounding, plateMin.y + 0.5f),
                    ImColor(1.0f, 1.0f, 1.0f, 0.20f * backAlpha),
                    1.0f
                );

                ImGui::SetCursorPos(ImVec2(platePadX, contentTop));

                ImGui::BeginChild("##LyricsScrollRegion", ImVec2(boxW - platePadX * 2.0f, contentH), false, ImGuiWindowFlags_NoScrollbar);
                float regionW = ImGui::GetWindowWidth() - 16.0f;

                if (lyr.status == LyricsStatus::Loading) {
                    lastActiveTime = now;
                    float midY = contentH * 0.42f;
                    const char* loadMsg = "Автопоиск текста (LRCLIB / Genius)...";
                    if (fontLyrics) ImGui::PushFont(fontLyrics);
                    ImVec2 mSz = ImGui::CalcTextSize(loadMsg);
                    ImGui::SetCursorPos(ImVec2((regionW - mSz.x) * 0.5f, midY));
                    ImGui::TextColored(ImVec4(0.88f, 0.92f, 0.98f, backAlpha), "%s", loadMsg);
                    if (fontLyrics) ImGui::PopFont();

                    // Анимированный индикатор волн поиска
                    ImDrawList* cdl = ImGui::GetWindowDrawList();
                    ImVec2 cwp = ImGui::GetWindowPos();
                    float waveBaseX = cwp.x + regionW * 0.5f - (scaleTier == 2 ? 34.0f : 24.0f);
                    float waveBaseY = cwp.y + midY + (scaleTier == 2 ? 56.0f : 44.0f);
                    float bSpacing = (scaleTier == 2) ? 15.0f : 11.0f;
                    float bWidth = (scaleTier == 2) ? 7.0f : 5.0f;
                    for (int b = 0; b < 5; ++b) {
                        float hWave = (scaleTier == 2 ? 8.0f : 6.0f) + (scaleTier == 2 ? 16.0f : 12.0f) * std::abs(std::sin(static_cast<float>(now) * 6.0f + b * 0.9f));
                        cdl->AddRectFilled(
                            ImVec2(waveBaseX + b * bSpacing, waveBaseY - hWave),
                            ImVec2(waveBaseX + b * bSpacing + bWidth, waveBaseY + hWave),
                            ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, backAlpha),
                            2.5f
                        );
                    }
                } else if (lyr.status == LyricsStatus::NotFound || lyr.status == LyricsStatus::None || lyr.lines.empty()) {
                    float midY = contentH * 0.42f;
                    const char* nfMsg = (lyr.status == LyricsStatus::None) ? "Загрузка информации о тексте..." : "Текст для этого трека не найден";
                    if (fontLyrics) ImGui::PushFont(fontLyrics);
                    ImVec2 mSz = ImGui::CalcTextSize(nfMsg);
                    ImGui::SetCursorPos(ImVec2((regionW - mSz.x) * 0.5f, midY));
                    ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.85f, backAlpha), "%s", nfMsg);
                    if (fontLyrics) ImGui::PopFont();
                } else if (lyr.status == LyricsStatus::FoundSynced) {
                    // --- РЕЖИМ КАРАОКЕ С ТАЙМКОДАМИ И КЛИКОМ ДЛЯ ПЕРЕМОТКИ ---
                    float curSec = audio.getPlaybackPosition();
                    int activeIdx = -1;
                    for (int i = 0; i < static_cast<int>(lyr.lines.size()); ++i) {
                        if (curSec + 0.12f >= lyr.lines[i].timeSec) {
                            activeIdx = i;
                        } else {
                            break;
                        }
                    }

                    ImGui::Dummy(ImVec2(1.0f, contentH * 0.35f));
                    float activeLineCenterY = 0.0f;

                    for (int i = 0; i < static_cast<int>(lyr.lines.size()); ++i) {
                        bool isCurrent = (i == activeIdx);
                        int dist = (activeIdx >= 0) ? std::abs(i - activeIdx) : i;

                        ImFont* curFont = isCurrent ? fontKaraoke : fontLyrics;
                        if (curFont) ImGui::PushFont(curFont);

                        const std::string& txt = lyr.lines[i].text;
                        ImVec2 wrapSz = ImGui::CalcTextSize(txt.c_str(), nullptr, false, regionW - 24.0f);
                        float extraRowPad = (scaleTier == 2) ? (isCurrent ? 28.0f : 20.0f) : ((scaleTier == 1) ? (isCurrent ? 24.0f : 16.0f) : (isCurrent ? 20.0f : 14.0f));
                        float rowH = wrapSz.y + extraRowPad;

                        float rowYStart = ImGui::GetCursorPosY();
                        if (isCurrent) {
                            activeLineCenterY = rowYStart + rowH * 0.5f;
                        }

                        ImGui::PushID(i);
                        ImVec2 rowScreenPos = ImGui::GetCursorScreenPos();
                        if (ImGui::InvisibleButton("##KaraokeLineBtn", ImVec2(regionW, rowH))) {
                            float dur = audio.getDuration();
                            if (dur > 0.0f) {
                                audio.seekTo(std::clamp(lyr.lines[i].timeSec / dur, 0.0f, 1.0f));
                            }
                        }
                        bool lineHovered = ImGui::IsItemHovered();
                        ImGui::PopID();

                        ImDrawList* cdl = ImGui::GetWindowDrawList();
                        if (isCurrent) {
                            float nextTime = (i + 1 < static_cast<int>(lyr.lines.size()))
                                ? lyr.lines[i + 1].timeSec
                                : (lyr.lines[i].timeSec + 5.0f);
                            float segDur = std::max(0.4f, nextTime - lyr.lines[i].timeSec);
                            float segProg = std::clamp((curSec - lyr.lines[i].timeSec) / segDur, 0.0f, 1.0f);

                            // Эфирный горизонтальный градиентный ореол за поющей строкой
                            float bloomH = rowH + 6.0f;
                            float bloomY = rowScreenPos.y - 3.0f;
                            float midX = rowScreenPos.x + regionW * 0.5f;
                            float glowHalfW = std::min(regionW * 0.46f, wrapSz.x * 0.60f + 36.0f);

                            cdl->AddRectFilledMultiColor(
                                ImVec2(midX - glowHalfW, bloomY),
                                ImVec2(midX, bloomY + bloomH),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.0f),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.0f)
                            );
                            cdl->AddRectFilledMultiColor(
                                ImVec2(midX, bloomY),
                                ImVec2(midX + glowHalfW, bloomY + bloomH),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.0f),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.0f),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha)
                            );

                            // Изящная лазерная полоска прогресса строки непосредственно под напечатанным текстом
                            float barW = std::min(wrapSz.x + 12.0f, regionW - 24.0f);
                            float barX = rowScreenPos.x + (regionW - barW) * 0.5f;
                            float barThickness = (scaleTier == 2) ? 3.0f : 2.0f;
                            float dotRadius = (scaleTier == 2) ? 3.5f : 2.5f;
                            float barY = rowScreenPos.y + rowH - barThickness;

                            cdl->AddRectFilled(
                                ImVec2(barX, barY),
                                ImVec2(barX + barW, barY + barThickness),
                                ImColor(1.0f, 1.0f, 1.0f, 0.12f * backAlpha),
                                1.0f
                            );
                            float progW = barW * segProg;
                            if (progW > 1.0f) {
                                cdl->AddRectFilled(
                                    ImVec2(barX, barY),
                                    ImVec2(barX + progW, barY + barThickness),
                                    ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.95f * backAlpha),
                                    1.0f
                                );
                                cdl->AddCircleFilled(
                                    ImVec2(barX + progW, barY + barThickness * 0.5f),
                                    dotRadius,
                                    ImColor(1.0f, 1.0f, 1.0f, 0.95f * backAlpha)
                                );
                            }
                        } else if (lineHovered) {
                            cdl->AddRectFilled(
                                ImVec2(rowScreenPos.x + 8.0f, rowScreenPos.y + 1.0f),
                                ImVec2(rowScreenPos.x + regionW - 8.0f, rowScreenPos.y + rowH - 1.0f),
                                ImColor(1.0f, 1.0f, 1.0f, 0.05f * backAlpha),
                                6.0f
                            );
                        }

                        float lineAlpha = isCurrent
                            ? 1.0f
                            : (lineHovered ? 0.95f : std::clamp(0.82f - static_cast<float>(dist) * 0.08f, 0.35f, 0.78f));

                        ImVec4 textCol = isCurrent
                            ? ImVec4(1.0f, 1.0f, 1.0f, backAlpha)
                            : ImVec4(0.88f, 0.92f, 0.98f, lineAlpha * backAlpha);

                        float textX = std::max(12.0f, (regionW - wrapSz.x) * 0.5f);
                        float textY = rowYStart + (rowH - wrapSz.y) * 0.5f - (isCurrent ? 2.0f : 0.0f);

                        // Напечатанный текст песни
                        ImGui::SetCursorPos(ImVec2(textX, textY));
                        ImGui::PushTextWrapPos(regionW - 12.0f);
                        ImGui::TextColored(textCol, "%s", txt.c_str());
                        ImGui::PopTextWrapPos();

                        float rowGap = (scaleTier == 2) ? 6.0f : 4.0f;
                        ImGui::SetCursorPosY(rowYStart + rowH + rowGap);

                        if (curFont) ImGui::PopFont();
                    }

                    ImGui::Dummy(ImVec2(1.0f, contentH * 0.40f));

                    // Плавная автопрокрутка караоке к текущей строке (если пользователь не крутит колесо вручную)
                    if (activeIdx >= 0 && io.MouseWheel == 0.0f) {
                        float targetScrollY = std::max(0.0f, activeLineCenterY - contentH * 0.45f);
                        float curScrollY = ImGui::GetScrollY();
                        float newScrollY = curScrollY + (targetScrollY - curScrollY) * std::min(1.0f, 7.5f * dt);
                        ImGui::SetScrollY(newScrollY);
                    }
                } else {
                    // --- ОБЫЧНЫЙ ТЕКСТ ПЕСНИ (GENIUS / LRCLIB PLAIN) ---
                    ImGui::Dummy(ImVec2(1.0f, 8.0f));
                    ImDrawList* cdl = ImGui::GetWindowDrawList();

                    for (size_t i = 0; i < lyr.lines.size(); ++i) {
                        const std::string& txt = lyr.lines[i].text;
                        if (txt.empty()) {
                            ImGui::Dummy(ImVec2(1.0f, 14.0f));
                            continue;
                        }
                        bool isSectionHeader = (txt.front() == '[' && txt.back() == ']');

                        if (isSectionHeader) {
                            // Секция-заголовок: крупный жирный шрифт + акцентная подчёркивающая линия
                            ImGui::Dummy(ImVec2(1.0f, 10.0f));
                            if (fontTitle) ImGui::PushFont(fontTitle);

                            ImVec2 wrapSz = ImGui::CalcTextSize(txt.c_str(), nullptr, false, regionW - 20.0f);
                            float textX = std::max(10.0f, (regionW - wrapSz.x) * 0.5f);

                            ImGui::SetCursorPosX(textX);
                            ImGui::PushTextWrapPos(regionW - 10.0f);
                            ImGui::TextColored(ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, backAlpha), "%s", txt.c_str());
                            ImGui::PopTextWrapPos();

                            // Акцентная подчёркивающая линия под заголовком секции
                            ImVec2 lineStart = ImGui::GetCursorScreenPos();
                            float lineW = std::min(wrapSz.x + 24.0f, regionW - 20.0f);
                            float lineX = lineStart.x + (regionW - lineW) * 0.5f - 8.0f;
                            float lineThick = (scaleTier == 2) ? 2.0f : 1.5f;
                            cdl->AddLine(
                                ImVec2(lineX, lineStart.y + 3.0f),
                                ImVec2(lineX + lineW, lineStart.y + 3.0f),
                                ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.50f * backAlpha),
                                lineThick
                            );
                            ImGui::Dummy(ImVec2(1.0f, 8.0f));

                            if (fontTitle) ImGui::PopFont();
                        } else {
                            // Обычная строка текста
                            if (fontLyrics) ImGui::PushFont(fontLyrics);

                            ImVec2 wrapSz = ImGui::CalcTextSize(txt.c_str(), nullptr, false, regionW - 20.0f);
                            float textX = std::max(10.0f, (regionW - wrapSz.x) * 0.5f);
                            float shadowY = ImGui::GetCursorPosY();

                            // Основной текст
                            ImGui::SetCursorPosX(textX);
                            ImGui::SetCursorPosY(shadowY);
                            ImGui::PushTextWrapPos(regionW - 10.0f);
                            ImGui::TextColored(ImVec4(0.94f, 0.96f, 1.0f, 0.95f * backAlpha), "%s", txt.c_str());
                            ImGui::PopTextWrapPos();

                            float rowGap = (scaleTier == 2) ? 6.0f : 4.0f;
                            ImGui::SetCursorPosY(shadowY + wrapSz.y + rowGap);

                            if (fontLyrics) ImGui::PopFont();
                        }
                    }
                    ImGui::Dummy(ImVec2(1.0f, 20.0f));
                }

                ImGui::EndChild();

                // Градиентные затухания сверху и снизу области прокрутки (растворение в матовую бумагу вкладыша)
                float fadeH = (scaleTier == 2) ? 38.0f : ((scaleTier == 1) ? 30.0f : 26.0f);
                float scrollContentTop = wp.y + contentTop;
                float scrollContentBot = wp.y + contentTop + contentH;
                // Верхний fade
                bdl->AddRectFilledMultiColor(
                    ImVec2(wp.x + 12.0f, scrollContentTop),
                    ImVec2(wp.x + boxW - 12.0f, scrollContentTop + fadeH),
                    ImColor(0.038f, 0.042f, 0.056f, 0.95f * backAlpha),
                    ImColor(0.038f, 0.042f, 0.056f, 0.95f * backAlpha),
                    ImColor(0.038f, 0.042f, 0.056f, 0.0f),
                    ImColor(0.038f, 0.042f, 0.056f, 0.0f)
                );
                // Нижний fade
                bdl->AddRectFilledMultiColor(
                    ImVec2(wp.x + 12.0f, scrollContentBot - fadeH),
                    ImVec2(wp.x + boxW - 12.0f, scrollContentBot),
                    ImColor(0.038f, 0.042f, 0.056f, 0.0f),
                    ImColor(0.038f, 0.042f, 0.056f, 0.0f),
                    ImColor(0.038f, 0.042f, 0.056f, 0.95f * backAlpha),
                    ImColor(0.038f, 0.042f, 0.056f, 0.95f * backAlpha)
                );

                // Аутентичный нижний колонтитул коллекционного CD-вкладыша
                float footerY = wp.y + boxH - footerH;

                // Линия-разделитель над футером
                bdl->AddLine(
                    ImVec2(wp.x + platePadX, footerY + 2.0f),
                    ImVec2(wp.x + boxW - platePadX, footerY + 2.0f),
                    ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.22f * backAlpha),
                    1.0f
                );

                // Левая часть: компакт-диск аудио спецификация
                float durSecTotal = audio.getDuration();
                char timeBuf[64];
                if (durSecTotal > 0.0f) {
                    int m = static_cast<int>(durSecTotal) / 60;
                    int s = static_cast<int>(durSecTotal) % 60;
                    std::snprintf(timeBuf, sizeof(timeBuf), "COMPACT DISC DIGITAL AUDIO • [%02d:%02d] STEREO", m, s);
                } else {
                    std::snprintf(timeBuf, sizeof(timeBuf), "COMPACT DISC DIGITAL AUDIO • STEREO");
                }
                bdl->AddText(
                    ImVec2(wp.x + platePadX + 2.0f, footerY + (scaleTier == 2 ? 12.0f : 9.0f)),
                    ImColor(0.68f, 0.74f, 0.86f, 0.65f * backAlpha),
                    timeBuf
                );

                // Правая часть: аутентичный процедурный штрих-код (CD Barcode)
                float bcW = (scaleTier == 2) ? 96.0f : ((scaleTier == 1) ? 82.0f : 72.0f);
                float bcH = (scaleTier == 2) ? 19.0f : ((scaleTier == 1) ? 17.0f : 15.0f);
                float bcX = wp.x + boxW - bcW - platePadX - 2.0f;
                float bcY = footerY + (scaleTier == 2 ? 10.0f : 8.0f);

                // Светлая подложка под штрихкодом
                bdl->AddRectFilled(
                    ImVec2(bcX - 3.0f, bcY - 1.0f),
                    ImVec2(bcX + bcW + 3.0f, bcY + bcH + 7.0f),
                    ImColor(0.92f, 0.94f, 0.98f, 0.75f * backAlpha),
                    2.0f
                );

                // Вертикальные полоски штрихкода (реалистичные чередующиеся штрихи)
                static const float s_barPattern[] = {
                    2.0f, 1.0f, 1.5f, 2.0f, 1.0f, 1.0f, 2.5f, 1.0f, 1.5f, 2.0f,
                    1.0f, 2.0f, 1.5f, 1.0f, 2.0f, 1.0f, 1.0f, 2.5f, 1.0f, 1.5f,
                    2.0f, 1.0f, 1.0f, 2.0f, 1.5f, 2.0f, 1.0f, 1.0f, 2.0f, 1.5f
                };
                float curBarX = bcX;
                float barScale = (scaleTier == 2) ? 1.33f : ((scaleTier == 1) ? 1.14f : 1.0f);
                float barSpacing = (scaleTier == 2) ? 1.2f : 0.9f;
                for (size_t b = 0; b < sizeof(s_barPattern)/sizeof(s_barPattern[0]); ++b) {
                    float wBar = s_barPattern[b] * barScale;
                    if (b % 2 == 0) {
                        bdl->AddRectFilled(
                            ImVec2(curBarX, bcY),
                            ImVec2(curBarX + wBar, bcY + bcH),
                            ImColor(0.05f, 0.05f, 0.08f, 0.85f * backAlpha)
                        );
                    }
                    curBarX += wBar + barSpacing;
                    if (curBarX > bcX + bcW) break;
                }
                // Цифры под штрихкодом
                bdl->AddText(
                    ImVec2(bcX + 2.0f, bcY + bcH - (scaleTier == 2 ? 1.0f : 2.0f)),
                    ImColor(0.08f, 0.08f, 0.12f, 0.80f * backAlpha),
                    "4 012968 0041"
                );

                if (fontBase) ImGui::PopFont();
                ImGui::End();
                ImGui::PopStyleColor(6);
            }
        }

        // ------------------ 2. ВСПЛЫВАЮЩЕЕ ОКНО ПРЯМО ПОД БОКСОМ + КУРСОР РУЛЕТКИ ------------------
        int bestHoverSlot = -1;
        float bestHoverWeight = 0.04f;
        if (g_inspectProgress < 0.1f) {
            for (size_t slot = 0; slot < g_visibleIndices.size(); ++slot) {
                int rIdx = g_visibleIndices[slot];
                if (rIdx >= 0 && rIdx < static_cast<int>(g_hoverWeights.size())) {
                    if (g_hoverWeights[rIdx] > bestHoverWeight) {
                        bestHoverWeight = g_hoverWeights[rIdx];
                        bestHoverSlot = static_cast<int>(slot);
                    }
                }
            }
        }

        if (bestHoverSlot != -1) {
            int rIdx = g_visibleIndices[bestHoverSlot];
            const auto& ht = g_playlist[rIdx];
            float alpha = std::clamp(bestHoverWeight, 0.0f, 1.0f);

            if (g_roulette.active) {
                glm::vec2 topAnchor = renderer.getBoxTopScreenPos(bestHoverSlot, bestHoverWeight, g_shelfScroll);
                ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
                float bounce = std::sin(static_cast<float>(now) * 18.0f) * 4.0f;
                ImVec2 tip(topAnchor.x, topAnchor.y - 10.0f + bounce);
                ImU32 ptrCol = ImColor(ht.accentColor.r, ht.accentColor.g, ht.accentColor.b, 1.0f);
                bgDl->AddTriangleFilled(
                    ImVec2(tip.x - 11.0f, tip.y - 16.0f),
                    ImVec2(tip.x + 11.0f, tip.y - 16.0f),
                    tip,
                    ptrCol
                );
                bgDl->AddCircleFilled(ImVec2(tip.x, tip.y - 20.0f), 4.5f, ImColor(1.0f, 1.0f, 1.0f, 0.95f));
            }

            glm::vec2 anchor = renderer.getBoxBottomScreenPos(bestHoverSlot, bestHoverWeight, g_shelfScroll);
            std::string titleText = truncateUtf8(ht.title, 30);
            std::string artistText = truncateUtf8(ht.artist, 32);

            ImVec2 tSz = ImGui::CalcTextSize(titleText.c_str());
            ImVec2 aSz = ImGui::CalcTextSize(artistText.c_str());
            float popupW = std::clamp((std::max)(tSz.x, aSz.x) + 32.0f, 160.0f, 360.0f);
            float popupH = 52.0f;

            float popX = std::clamp(anchor.x - popupW * 0.5f, 16.0f, static_cast<float>(w) - popupW - 16.0f);
            float popY = anchor.y + 18.0f + (1.0f - alpha) * 10.0f;

            ImGui::SetNextWindowPos(ImVec2(popX, popY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(popupW, popupH), ImGuiCond_Always);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.13f, 0.94f * alpha));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(ht.accentColor.r, ht.accentColor.g, ht.accentColor.b, 0.65f * alpha));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);

            ImGui::Begin("##UnderBoxHoverPopup", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);

            float lineH = ImGui::GetTextLineHeight();
            float totalTextH = lineH * 2.0f + 2.0f;
            float startY = (popupH - totalTextH) * 0.5f;

            ImGui::SetCursorPos(ImVec2((popupW - tSz.x) * 0.5f, startY));
            ImGui::TextColored(ImVec4(0.98f, 0.99f, 1.0f, alpha), "%s", titleText.c_str());

            ImGui::SetCursorPos(ImVec2((popupW - aSz.x) * 0.5f, startY + lineH + 2.0f));
            ImGui::TextColored(ImVec4(0.68f, 0.74f, 0.84f, alpha), "%s", artistText.c_str());

            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

        // ------------------ 3. ВЕРХНЯЯ ОБЛАСТЬ: ЛЕВАЯ КНОПКА РУЛЕТКИ + ЦЕНТРАЛЬНЫЙ ПОИСК + КНОПКИ ОКНА СПРАВА ------------------
        const float topBarY = 16.0f;
        const float topBarH = 46.0f;
        const float itemH = 30.0f;
        const float itemY = (topBarH - itemH) * 0.5f;

        // Левая капсула: кнопка случайного включения песни (Рулетка), стилизованная точно так же, как кнопки управления окном
        const float leftCtrlW = 50.0f;
        ImGui::SetNextWindowPos(ImVec2(18.0f, topBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(leftCtrlW, topBarH), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::Begin("##RouletteControlsPill", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar);
        {
            const float btnW = 32.0f;
            const float btnH = 30.0f;
            const float bx = (leftCtrlW - btnW) * 0.5f;
            const float by = (topBarH - btnH) * 0.5f;
            glm::vec3 rAcc = renderer.getAccentAtScreenPos(glm::vec2(25.0f + bx, topBarY + by));
            ImVec4 accCol(rAcc.r, rAcc.g, rAcc.b, 1.0f);

            ImGui::SetCursorPos(ImVec2(bx, by));
            if (IconButton("##TopRouletteBtn", IconType::Dice, ImVec2(btnW, btnH), g_roulette.active, accCol, 1.0f)) {
                startRandomRoulette(g_visibleIndices, audio, discFlightProgress);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Случайный трек");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);

        // Вторая левая капсула: кнопка Настроек (Шестерёнка), прямо под кнопкой рулетки
        const float setPillY = topBarY + topBarH + 8.0f;
        ImGui::SetNextWindowPos(ImVec2(18.0f, setPillY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(leftCtrlW, topBarH), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::Begin("##SettingsControlsPill", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar);
        {
            const float btnW = 32.0f;
            const float btnH = 30.0f;
            const float bx = (leftCtrlW - btnW) * 0.5f;
            const float by = (topBarH - btnH) * 0.5f;
            glm::vec3 sAcc = renderer.getAccentAtScreenPos(glm::vec2(25.0f + bx, setPillY + by));
            ImVec4 accCol(sAcc.r, sAcc.g, sAcc.b, 1.0f);

            ImGui::SetCursorPos(ImVec2(bx, by));
            if (IconButton("##TopSettingsBtn", IconType::Settings, ImVec2(btnW, btnH), g_showSettingsWindow, accCol, 1.0f)) {
                g_showSettingsWindow = !g_showSettingsWindow;
                lastActiveTime = now;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Настройки");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);

        // Плавная анимация шестерёнки и появления окна настроек
        {
            float targetSetProg = g_showSettingsWindow ? 1.0f : 0.0f;
            float setSpeed = g_showSettingsWindow ? 18.0f : 20.0f;
            g_settingsAnimProgress += (targetSetProg - g_settingsAnimProgress) * std::min(1.0f, setSpeed * dt);
            if (!g_showSettingsWindow && g_settingsAnimProgress < 0.005f) {
                g_settingsAnimProgress = 0.0f;
            }

            float targetGear = g_showSettingsWindow ? 3.14159265f : 0.0f;
            g_settingsGearRotation += (targetGear - g_settingsGearRotation) * std::min(1.0f, 18.0f * dt);
        }

        // ------------------ ПЛАВНО ВСПЛЫВАЮЩЕЕ ОКНО НАСТРОЕК ------------------
        if (g_settingsAnimProgress > 0.005f) {
            float setEase = 1.0f - std::pow(1.0f - g_settingsAnimProgress, 3.0f);
            float setWinW = 350.0f;
            float setWinH = 195.0f;
            float startX = 18.0f + leftCtrlW + 10.0f;
            float curWinX = startX - (1.0f - setEase) * 20.0f;
            float curWinY = setPillY;

            ImGui::SetNextWindowPos(ImVec2(curWinX, curWinY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(setWinW, setWinH), ImGuiCond_Always);

            glm::vec3 setAcc = renderer.getAccentAtScreenPos(glm::vec2(curWinX + setWinW * 0.5f, curWinY + setWinH * 0.5f));

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 9.0f));

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.13f, 0.96f * setEase));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(setAcc.r, setAcc.g, setAcc.b, 0.40f * setEase));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.97f, 1.0f, setEase));

            ImGui::Begin("##PlayerSettingsFlyout", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
            {
                ImDrawList* sdl = ImGui::GetWindowDrawList();
                ImVec2 swp = ImGui::GetWindowPos();

                // Шапка настроек
                ImGui::SetCursorPos(ImVec2(18.0f, 14.0f));
                ImGui::TextColored(ImVec4(setAcc.r, setAcc.g, setAcc.b, setEase), "НАСТРОЙКИ");

                // Кнопка закрытия [X]
                ImGui::SetCursorPos(ImVec2(setWinW - 32.0f, 10.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f * setEase));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.22f, 0.28f, 0.85f * setEase));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.15f, 0.20f, 0.95f * setEase));
                if (ImGui::Button("X##CloseSettings", ImVec2(22.0f, 22.0f))) {
                    g_showSettingsWindow = false;
                    lastActiveTime = now;
                }
                ImGui::PopStyleColor(3);

                // Акцентная линия разделителя
                sdl->AddLine(
                    ImVec2(swp.x + 16.0f, swp.y + 40.0f),
                    ImVec2(swp.x + setWinW - 16.0f, swp.y + 40.0f),
                    ImColor(setAcc.r, setAcc.g, setAcc.b, 0.25f * setEase),
                    1.0f
                );

                ImGui::SetCursorPosY(48.0f);

                // 1. Сглаживание (Anti-aliasing)
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, setEase), "Сглаживание сцены:");
                AAMode curAA = renderer.getAAMode();
                const float segW = (setWinW - 36.0f - 12.0f) / 3.0f;
                const float segH = 26.0f;

                auto renderSegBtn = [&](const char* label, const char* id, bool isSel, AAMode targetMode) {
                    ImVec4 btnBg = isSel
                        ? ImVec4(setAcc.r, setAcc.g, setAcc.b, 0.85f * setEase)
                        : ImVec4(1.0f, 1.0f, 1.0f, 0.07f * setEase);
                    ImVec4 textCol = isSel
                        ? ImVec4(1.0f, 1.0f, 1.0f, setEase)
                        : ImVec4(0.78f, 0.82f, 0.90f, 0.85f * setEase);

                    ImGui::PushStyleColor(ImGuiCol_Button, btnBg);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isSel ? btnBg : ImVec4(setAcc.r, setAcc.g, setAcc.b, 0.35f * setEase));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(setAcc.r, setAcc.g, setAcc.b, 0.95f * setEase));
                    ImGui::PushStyleColor(ImGuiCol_Text, textCol);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

                    if (ImGui::Button(id, ImVec2(segW, segH))) {
                        renderer.setAAMode(targetMode);
                        g_savedAAMode = targetMode;
                        saveSettingsToDisk(audio);
                        lastActiveTime = now;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(4);
                };

                renderSegBtn("2x MSAA", "2x MSAA##AA_MSAA", curAA == AAMode::MSAA, AAMode::MSAA);
                ImGui::SameLine(0, 6.0f);
                renderSegBtn("FXAA", "FXAA##AA_FXAA", curAA == AAMode::FXAA, AAMode::FXAA);
                ImGui::SameLine(0, 6.0f);
                renderSegBtn("Выкл", "Выкл##AA_OFF", curAA == AAMode::Off, AAMode::Off);

                ImGui::Spacing();

                // 2. Отражения под боксами
                bool reflOn = renderer.isReflectionsEnabled();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, setEase), "Отражения под боксами:");
                ImGui::SameLine(setWinW - 64.0f);
                if (AnimatedToggle("##ReflToggle", &reflOn, ImVec4(setAcc.r, setAcc.g, setAcc.b, 1.0f), setEase)) {
                    renderer.setReflectionsEnabled(reflOn);
                    g_savedReflections = reflOn;
                    saveSettingsToDisk(audio);
                    lastActiveTime = now;
                }

                ImGui::Spacing();

                // 3. Жирный шрифт интерфейса
                bool boldOn = g_boldInterfaceFont;
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, setEase), "Жирный шрифт интерфейса:");
                ImGui::SameLine(setWinW - 64.0f);
                if (AnimatedToggle("##BoldToggle", &boldOn, ImVec4(setAcc.r, setAcc.g, setAcc.b, 1.0f), setEase)) {
                    g_boldInterfaceFont = boldOn;
                    saveSettingsToDisk(audio);
                    lastActiveTime = now;
                }
            }
            ImGui::End();
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);

            // Клик снаружи закрывает окно
            if (ImGui::IsMouseClicked(0)) {
                ImVec2 m = io.MousePos;
                bool inSetWin = (m.x >= curWinX && m.x <= curWinX + setWinW && m.y >= curWinY && m.y <= curWinY + setWinH);
                bool inSetPill = (m.x >= 18.0f && m.x <= 18.0f + leftCtrlW && m.y >= setPillY && m.y <= setPillY + topBarH);
                if (!inSetWin && !inSetPill) {
                    g_showSettingsWindow = false;
                    lastActiveTime = now;
                }
            }
        }

        const float topBarW = 820.0f;
        ImGui::SetNextWindowPos(ImVec2((w - topBarW) * 0.5f, topBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(topBarW, topBarH), ImGuiCond_Always);

        glm::vec3 topBarAcc = renderer.getAccentAtScreenPos(glm::vec2(w * 0.5f, topBarY + topBarH * 0.5f));

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.52f, 0.56f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(topBarAcc.r, topBarAcc.g, topBarAcc.b, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(topBarAcc.r, topBarAcc.g, topBarAcc.b, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(topBarAcc.r * 0.8f, topBarAcc.g * 0.8f, topBarAcc.b * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));

        float fontH = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, (itemH - fontH) * 0.5f));

        ImGui::Begin("TopBarClean", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        float curX = 10.0f;
        ImVec4 accCol(topBarAcc.r, topBarAcc.g, topBarAcc.b, 1.0f);

        ImGui::SetCursorPos(ImVec2(curX, itemY));
        if (ImGui::Button("+ Файлы", ImVec2(74.0f, itemH))) {
            openFileDialog(hwnd);
        }
        curX += 74.0f + 6.0f;

        ImGui::SetCursorPos(ImVec2(curX, itemY));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.20f));
        if (ImGui::Button("+ Папка", ImVec2(74.0f, itemH))) {
            openFolderDialog(hwnd);
        }
        ImGui::PopStyleColor(2);
        curX += 74.0f + 8.0f;

        bool ymAuth = YandexMusicManager::instance().isAuthenticated();
        std::string ymUser = YandexMusicManager::instance().getUserName();
        const float ymBtnW = 34.0f;
        const float toggleW = 144.0f;

        std::ostringstream countSS;
        countSS << g_visibleIndices.size() << " / " << (g_playlist.size() + g_pendingLoadQueue.size());
        std::string countStr = countSS.str();
        ImVec2 countSz = ImGui::CalcTextSize(countStr.c_str());

        bool hasClearBtn = (g_searchQuery[0] != '\0');
        float rightReserved = (hasClearBtn ? 32.0f : 0.0f) + toggleW + 8.0f + ymBtnW + 10.0f + countSz.x + 18.0f;
        float searchW = topBarW - curX - rightReserved;
        if (searchW < 120.0f) searchW = 120.0f;

        ImGui::SetCursorPos(ImVec2(curX, itemY));
        ImGui::SetNextItemWidth(searchW);
        if (g_focusSearchInput) {
            ImGui::SetKeyboardFocusHere();
            g_focusSearchInput = false;
        }
        const char* searchHint = (g_searchSource == SearchSource::Yandex)
            ? "Поиск в Яндекс Музыке..."
            : "Поиск по медиатеке...";
        if (ImGui::InputTextWithHint("##SearchInput", searchHint, g_searchQuery, sizeof(g_searchQuery), ImGuiInputTextFlags_EnterReturnsTrue)) {
            g_searchDirty = true;
            if (g_searchSource == SearchSource::Yandex && g_searchQuery[0] != '\0') {
                YandexMusicManager::instance().searchAsync(g_searchQuery);
            }
        } else if (ImGui::IsItemEdited()) {
            g_searchDirty = true;
        }
        ImVec2 searchMin = ImGui::GetItemRectMin();
        ImVec2 searchMax = ImGui::GetItemRectMax();
        curX += searchW + 6.0f;

        if (hasClearBtn) {
            ImGui::SetCursorPos(ImVec2(curX, itemY));
            if (IconButton("##ClearSearch", IconType::CloseWin, ImVec2(24.0f, itemH), false, ImVec4(1.0f, 1.0f, 1.0f, 0.75f), 1.0f)) {
                g_searchQuery[0] = '\0';
                g_searchDirty = true;
                YandexMusicManager::instance().clearSearchResults();
            }
            curX += 24.0f + 6.0f;
        }

        // Переключатель источника поиска (Локально / Яндекс) — СПРАВА от строки поиска
        ImGui::SetCursorPos(ImVec2(curX, itemY));
        if (DrawSearchSourceToggle(g_searchSource, accCol, itemH)) {
            g_searchDirty = true;
            lastActiveTime = now;
            if (g_searchSource == SearchSource::Yandex && g_searchQuery[0] != '\0') {
                YandexMusicManager::instance().searchAsync(g_searchQuery);
            }
        }
        curX += toggleW + 8.0f;

        // Кнопка Яндекс Музыки (аккуратная круглая иконка со статусом)
        ImGui::SetCursorPos(ImVec2(curX, itemY));
        if (DrawYandexAuthButton("##YmTopAuthBtn", ImVec2(ymBtnW, itemH), ymAuth, ymUser, 1.0f)) {
            g_showYmAuthModal = true;
        }

        ImGui::SetCursorPos(ImVec2(topBarW - countSz.x - 14.0f, (topBarH - countSz.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "%s", countStr.c_str());

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(8);

        // ------------------ ОНЛАЙН-РЕЗУЛЬТАТЫ ЯНДЕКС МУЗЫКИ (ВСПЛЫВАЮЩИЙ СПИСОК) ------------------
        auto ymResults = YandexMusicManager::instance().getSearchResults();
        bool isYmSearching = YandexMusicManager::instance().isSearching();

        if (g_searchSource == SearchSource::Yandex && g_searchQuery[0] != '\0' && (isYmSearching || !ymResults.empty())) {
            float popupW = std::clamp(searchMax.x - searchMin.x + 160.0f, 540.0f, 720.0f);
            float popupH = std::clamp(64.0f + static_cast<float>(ymResults.size()) * 48.0f, 100.0f, 400.0f);

            ImGui::SetNextWindowPos(ImVec2(searchMin.x - 30.0f, searchMax.y + 8.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(popupW, popupH), ImGuiCond_Always);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.13f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.40f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));

            ImGuiWindowFlags ddFlags = ImGuiWindowFlags_NoTitleBar |
                                      ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoNavFocus |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;

            ImGui::Begin("##YmOnlineSearchResultsDropdown", nullptr, ddFlags);

            ImDrawList* ddl = ImGui::GetWindowDrawList();
            ImVec2 dropHeaderP = ImGui::GetCursorScreenPos();

            // Круглая иконка Яндекса в шапке выпадающего списка
            ImVec2 dIconC(dropHeaderP.x + 10.0f, dropHeaderP.y + 10.0f);
            ddl->AddCircleFilled(dIconC, 8.5f, ImColor(0.988f, 0.235f, 0.118f, 1.0f), 24);
            if (g_fontMainBold) {
                const char* letter = "Я";
                ImVec2 tSz = g_fontMainBold->CalcTextSizeA(12.5f, FLT_MAX, -1.0f, letter);
                ddl->AddText(g_fontMainBold, 12.5f,
                             ImVec2(dIconC.x - tSz.x * 0.5f, dIconC.y - tSz.y * 0.5f - 0.5f),
                             ImColor(1.0f, 1.0f, 1.0f, 1.0f), letter);
            }

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 26.0f);
            ImGui::TextColored(ImVec4(0.98f, 0.99f, 1.0f, 1.0f), "Яндекс Музыка");
            ImGui::SameLine();
            if (isYmSearching) {
                float pulse = 0.5f + 0.5f * sinf(static_cast<float>(glfwGetTime()) * 5.0f);
                ImVec4 pulseCol = ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.6f + 0.4f * pulse);
                ImGui::TextColored(pulseCol, "Поиск по каталогу...");
            } else {
                ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.78f, 1.0f), "(найдено: %d)", static_cast<int>(ymResults.size()));
            }

            ImGui::SameLine(popupW - 140.0f);
            ImGui::TextColored(ImVec4(0.48f, 0.52f, 0.64f, 0.90f), "[Esc] Закрыть");

            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginChild("##YmTrackListScroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& t : ymResults) {
                int shelfIdx = findTrackByYmId(t.id);
                bool onShelf = (shelfIdx >= 0);
                bool isDownloading = YandexMusicManager::instance().isTrackDownloading(t.id);
                std::string durStr = formatTime(static_cast<float>(t.duration));

                std::string itemTitle = truncateUtf8(t.title, 34);
                std::string itemArtist = truncateUtf8(t.artist + (t.album.empty() ? "" : " - " + t.album), 38);

                std::string rowId = "##ym_track_" + t.id;
                ImGui::PushID(rowId.c_str());

                float availW = ImGui::GetContentRegionAvail().x;
                float rowH = 50.0f;
                ImVec2 rowPos = ImGui::GetCursorScreenPos();

                bool rowClicked = ImGui::InvisibleButton("##RowClick", ImVec2(availW, rowH));
                bool isRowHovered = ImGui::IsItemHovered();

                ImDrawList* listDl = ImGui::GetWindowDrawList();

                // Отрисовка фона строки с эффектом подсветки при наведении
                if (isRowHovered) {
                    listDl->AddRectFilled(rowPos, ImVec2(rowPos.x + availW, rowPos.y + rowH),
                                          ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.16f), 8.0f);
                    listDl->AddRect(rowPos, ImVec2(rowPos.x + availW, rowPos.y + rowH),
                                    ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.32f), 8.0f, 0, 1.0f);
                } else {
                    listDl->AddRectFilled(rowPos, ImVec2(rowPos.x + availW, rowPos.y + rowH),
                                          ImColor(1.0f, 1.0f, 1.0f, 0.025f), 8.0f);
                }

                // Загрузка миниатюры обложки при наличии
                GLuint thumbTex = 0;
                if (!t.thumbPath.empty()) {
                    auto it = s_ymThumbTextures.find(t.thumbPath);
                    if (it != s_ymThumbTextures.end()) {
                        thumbTex = it->second;
                    } else {
                        std::ifstream tf(t.thumbPath, std::ios::binary | std::ios::ate);
                        if (tf.is_open()) {
                            auto tsz = tf.tellg();
                            if (tsz > 200 && tsz < 5 * 1024 * 1024) {
                                tf.seekg(0, std::ios::beg);
                                std::vector<uint8_t> tbuf(static_cast<size_t>(tsz));
                                if (tf.read(reinterpret_cast<char*>(tbuf.data()), tsz)) {
                                    thumbTex = MetadataParser::uploadTextureFromMemory(tbuf.data(), static_cast<int>(tbuf.size()), nullptr);
                                    s_ymThumbTextures[t.thumbPath] = thumbTex;
                                }
                            }
                        }
                    }
                }

                // Обложка трека (38x38 со скруглением)
                ImVec2 thumbMin(rowPos.x + 8.0f, rowPos.y + (rowH - 38.0f) * 0.5f);
                ImVec2 thumbMax(thumbMin.x + 38.0f, thumbMin.y + 38.0f);
                if (thumbTex != 0) {
                    listDl->AddImageRounded((ImTextureID)(intptr_t)thumbTex, thumbMin, thumbMax,
                                           ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, 6.0f);
                    listDl->AddRect(thumbMin, thumbMax, ImColor(1.0f, 1.0f, 1.0f, 0.14f), 6.0f, 0, 1.0f);
                } else {
                    listDl->AddRectFilled(thumbMin, thumbMax, ImColor(0.11f, 0.13f, 0.18f, 0.95f), 6.0f);
                    listDl->AddRect(thumbMin, thumbMax, ImColor(1.0f, 1.0f, 1.0f, 0.10f), 6.0f, 0, 1.0f);
                    ImVec2 discC(thumbMin.x + 19.0f, thumbMin.y + 19.0f);
                    listDl->AddCircleFilled(discC, 12.0f, ImColor(0.18f, 0.20f, 0.28f, 1.0f), 24);
                    listDl->AddCircleFilled(discC, 3.5f, ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.85f), 16);
                }

                // Текст названия и исполнителя
                float textLeft = rowPos.x + 56.0f;
                if (g_fontMainBold) {
                    listDl->AddText(g_fontMainBold, 15.0f, ImVec2(textLeft, rowPos.y + 7.0f),
                                    ImColor(0.96f, 0.98f, 1.0f, 1.0f), itemTitle.c_str());
                } else {
                    listDl->AddText(ImVec2(textLeft, rowPos.y + 7.0f),
                                    ImColor(0.96f, 0.98f, 1.0f, 1.0f), itemTitle.c_str());
                }

                listDl->AddText(ImVec2(textLeft, rowPos.y + 27.0f),
                                ImColor(0.65f, 0.70f, 0.82f, 0.92f), itemArtist.c_str());

                // Длительность трека
                ImVec2 durSz = ImGui::CalcTextSize(durStr.c_str());
                float rightMargin = 12.0f;
                float btnW = 96.0f;
                float durX = rowPos.x + availW - rightMargin - btnW - 14.0f - durSz.x;
                listDl->AddText(ImVec2(durX, rowPos.y + (rowH - durSz.y) * 0.5f),
                                ImColor(0.55f, 0.60f, 0.72f, 1.0f), durStr.c_str());

                // Кнопка статуса справа
                ImVec2 btnMin(rowPos.x + availW - rightMargin - btnW, rowPos.y + (rowH - 28.0f) * 0.5f);
                ImVec2 btnMax(btnMin.x + btnW, btnMin.y + 28.0f);

                if (isDownloading) {
                    listDl->AddRectFilled(btnMin, btnMax, ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.35f), 14.0f);
                    listDl->AddRect(btnMin, btnMax, ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.65f), 14.0f, 0, 1.0f);
                    const char* txt = "Загрузка...";
                    ImVec2 txtSz = ImGui::CalcTextSize(txt);
                    listDl->AddText(ImVec2(btnMin.x + (btnW - txtSz.x) * 0.5f, btnMin.y + (28.0f - txtSz.y) * 0.5f),
                                    ImColor(1.0f, 1.0f, 1.0f, 1.0f), txt);
                } else if (onShelf) {
                    listDl->AddRectFilled(btnMin, btnMax, ImColor(0.18f, 0.75f, 0.40f, 0.25f), 14.0f);
                    listDl->AddRect(btnMin, btnMax, ImColor(0.25f, 0.85f, 0.45f, 0.50f), 14.0f, 0, 1.0f);
                    const char* txt = "На полке";
                    ImVec2 txtSz = ImGui::CalcTextSize(txt);
                    listDl->AddText(ImVec2(btnMin.x + (btnW - txtSz.x) * 0.5f, btnMin.y + (28.0f - txtSz.y) * 0.5f),
                                    ImColor(0.40f, 0.95f, 0.55f, 1.0f), txt);
                } else {
                    ImU32 btnBg = isRowHovered ? ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.35f)
                                               : ImColor(1.0f, 1.0f, 1.0f, 0.08f);
                    listDl->AddRectFilled(btnMin, btnMax, btnBg, 14.0f);
                    listDl->AddRect(btnMin, btnMax, ImColor(1.0f, 1.0f, 1.0f, 0.16f), 14.0f, 0, 1.0f);
                    const char* txt = "Скачать";
                    ImVec2 txtSz = ImGui::CalcTextSize(txt);
                    listDl->AddText(ImVec2(btnMin.x + (btnW - txtSz.x) * 0.5f, btnMin.y + (28.0f - txtSz.y) * 0.5f),
                                    ImColor(0.92f, 0.95f, 1.0f, 1.0f), txt);
                }

                if (rowClicked) {
                    if (onShelf) {
                        playTrackAndFetchLyrics(shelfIdx, audio, discFlightProgress);
                        focusTrackOnShelf(shelfIdx, g_visibleIndices);
                    } else if (!isDownloading) {
                        YandexMusicManager::instance().downloadTrackAsync(t);
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }

        // Правая капсула управления окном
        const float winCtrlW = 126.0f;
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(w) - winCtrlW - 18.0f, topBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(winCtrlW, topBarH), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::Begin("##WindowControlsPill", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar);
        {
            const float btnW = 32.0f;
            const float btnH = 30.0f;
            const float by = (topBarH - btnH) * 0.5f;
            glm::vec3 winBtnAcc = renderer.getAccentAtScreenPos(glm::vec2(static_cast<float>(w) - 60.0f, by + btnH * 0.5f));
            ImVec4 accCol(winBtnAcc.r, winBtnAcc.g, winBtnAcc.b, 1.0f);

            ImGui::SetCursorPos(ImVec2(9.0f, by));
            if (IconButton("##WinMin", IconType::Minimize, ImVec2(btnW, btnH), false, accCol, 1.0f)) {
                PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            }

            bool isMax = (IsZoomed(hwnd) != FALSE);
            ImGui::SetCursorPos(ImVec2(9.0f + btnW + 6.0f, by));
            if (IconButton("##WinMax", isMax ? IconType::Restore : IconType::Maximize, ImVec2(btnW, btnH), false, accCol, 1.0f)) {
                PostMessageW(hwnd, WM_SYSCOMMAND, isMax ? SC_RESTORE : SC_MAXIMIZE, 0);
            }

            ImGui::SetCursorPos(ImVec2(9.0f + (btnW + 6.0f) * 2.0f, by));
            if (IconButton("##WinClose", IconType::CloseWin, ImVec2(btnW, btnH), false, accCol, 1.0f)) {
                PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);

        if (!io.WantCaptureMouse && mouseY >= 0.0 && mouseY < 72.0) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                PostMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        }

        // Подсказка на пустом экране
        if (g_playlist.empty() && g_pendingLoadQueue.empty()) {
            float hintW = 440.0f, hintH = 88.0f;
            ImGui::SetNextWindowPos(ImVec2((w - hintW) * 0.5f, (h - hintH) * 0.45f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(hintW, hintH), ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.88f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
            ImGui::Begin("EmptyHint", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

            const char* line1 = "Перетащите аудиофайлы или папку с музыкой в окно";
            const char* line2 = "или нажмите «+ Файлы» / «+ Папка» на верхней панели";
            ImVec2 s1 = ImGui::CalcTextSize(line1);
            ImVec2 s2 = ImGui::CalcTextSize(line2);
            float blockH = s1.y + s2.y + 6.0f;
            float sy = (hintH - blockH) * 0.5f;

            ImGui::SetCursorPos(ImVec2((hintW - s1.x) * 0.5f, sy));
            ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.99f, 1.0f), "%s", line1);
            ImGui::SetCursorPos(ImVec2((hintW - s2.x) * 0.5f, sy + s1.y + 6.0f));
            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.74f, 1.0f), "%s", line2);

            ImGui::End();
            ImGui::PopStyleColor(2);
        }

        // ================== 4. НИЖНИЙ БАР ПЛЕЕРА ==================
        if (hudAlpha > 0.01f && !g_playlist.empty()) {
            const float dockW = std::min(880.0f, static_cast<float>(w) - 32.0f);
            const float dockH = 84.0f;
            const float slideY = (1.0f - hudAlpha) * 45.0f;
            const float dockX = (w - dockW) * 0.5f;
            const float dockY = static_cast<float>(h) - dockH - 18.0f + slideY;

            glm::vec3 dockAccent = renderer.getAccentAtScreenPos(glm::vec2(dockX + dockW * 0.5f, dockY + dockH * 0.5f));
            ImVec4 barBgColor(
                dockAccent.r * 0.14f + 0.06f,
                dockAccent.g * 0.14f + 0.07f,
                dockAccent.b * 0.14f + 0.09f,
                0.92f * hudAlpha
            );
            ImVec4 barBorderColor(
                dockAccent.r * 0.55f + 0.30f,
                dockAccent.g * 0.55f + 0.30f,
                dockAccent.b * 0.55f + 0.30f,
                0.35f * hudAlpha
            );
            ImVec4 accentRGBA(dockAccent.r, dockAccent.g, dockAccent.b, hudAlpha);

            auto getUICol = [&](float localX, float localY) -> ImVec4 {
                glm::vec3 c = renderer.getAccentAtScreenPos(glm::vec2(dockX + localX, dockY + localY));
                return ImVec4(c.r, c.g, c.b, hudAlpha);
            };

            ImGui::SetNextWindowPos(ImVec2(dockX, dockY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(dockW, dockH), ImGuiCond_Always);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, barBgColor);
            ImGui::PushStyleColor(ImGuiCol_Border, barBorderColor);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, hudAlpha));

            ImGui::Begin("LiquidGlassDockBar", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

            const auto& curTrack = g_playlist[displayTrackIdx];
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 winPos = ImGui::GetWindowPos();

            const float coverSz = 56.0f;
            const float coverX = 16.0f;
            const float coverY = (dockH - coverSz) * 0.5f;

            ImGui::SetCursorPos(ImVec2(coverX, coverY));
            ImGui::InvisibleButton("##RoundedCoverBtn", ImVec2(coverSz, coverSz));
            if (ImGui::IsItemClicked()) {
                // Клик по обложке в док-баре выдвигает 3D-бокс и переворачивает его задней крышкой с текстом песни!
                g_lyricsInspectMode = !g_lyricsInspectMode;
                g_inspectShowFront = false;
                triggerAutoLyricsForTrack(displayTrackIdx, audio);
                focusTrackOnShelf(displayTrackIdx, g_visibleIndices);
            }
            if (curTrack.coverTex != 0) {
                ImVec2 pMin(winPos.x + coverX, winPos.y + coverY);
                ImVec2 pMax(pMin.x + coverSz, pMin.y + coverSz);
                dl->AddImageRounded(
                    (ImTextureID)(intptr_t)curTrack.coverTex,
                    pMin,
                    pMax,
                    ImVec2(0, 1),
                    ImVec2(1, 0),
                    ImColor(1.0f, 1.0f, 1.0f, hudAlpha),
                    12.0f
                );
                dl->AddRect(pMin, pMax, ImColor(1.0f, 1.0f, 1.0f, 0.22f * hudAlpha), 12.0f, 0, 1.0f);
            }

            float lineH = ImGui::GetTextLineHeight();
            float textBlockH = lineH * 2.0f + 4.0f;
            float textStartY = (dockH - textBlockH) * 0.5f;
            float textX = coverX + coverSz + 14.0f;

            float eqWidth = 18.0f;
            ImVec2 eqBase(winPos.x + textX, winPos.y + textStartY + lineH - 2.0f);
            for (int b = 0; b < 4; ++b) {
                float barRatio = audio.isPlaying()
                    ? (0.28f + 0.72f * std::abs(std::sin(static_cast<float>(now) * (5.5f + b * 1.7f) + b * 1.1f)))
                    : 0.22f;
                float bh = barRatio * (lineH - 3.0f);
                float bx = eqBase.x + b * 4.2f;
                dl->AddRectFilled(ImVec2(bx, eqBase.y - bh), ImVec2(bx + 2.8f, eqBase.y), ImColor(getUICol(textX + b * 4.2f, textStartY)), 1.2f);
            }

            std::string safeTitle = truncateUtf8(curTrack.title, 20);
            std::string subLine = truncateUtf8(curTrack.artist, 20) + "  |  " + curTrack.formatBadge;

            ImGui::SetCursorPos(ImVec2(textX + eqWidth + 4.0f, textStartY));
            ImGui::TextColored(ImVec4(0.98f, 0.99f, 1.0f, hudAlpha), "%s", safeTitle.c_str());

            ImGui::SetCursorPos(ImVec2(textX, textStartY + lineH + 4.0f));
            ImGui::TextColored(ImVec4(0.70f, 0.76f, 0.86f, hudAlpha), "%s", subLine.c_str());

            // Центральные кнопки (Shuffle, Prev, Play/Pause, Next, Repeat, Roulette, Lyrics Back-of-Box)
            const float centerAxisX = dockW * 0.5f;
            const float row1CenterY = 26.0f;
            const float secBtnSz = 30.0f;
            const float mainBtnSz = 36.0f;
            const float btnGap = 8.0f;
            const float totalBtnsW = secBtnSz * 5.0f + mainBtnSz + btnGap * 5.0f;
            float bx = centerAxisX - totalBtnsW * 0.5f;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##ShuffleBtn", IconType::Shuffle, ImVec2(secBtnSz, secBtnSz), g_isShuffle, getUICol(bx + secBtnSz * 0.5f, row1CenterY), hudAlpha)) {
                g_isShuffle = !g_isShuffle;
            }
            bx += secBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##PrevBtn", IconType::Prev, ImVec2(secBtnSz, secBtnSz), false, getUICol(bx + secBtnSz * 0.5f, row1CenterY), hudAlpha)) {
                if (audio.getPlaybackPosition() > 3.0f) {
                    audio.seekTo(0.0f);
                } else {
                    const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                    int prevIdx = getPrevTrackIndex(listToUse);
                    if (prevIdx >= 0) {
                        playTrackAndFetchLyrics(prevIdx, audio, discFlightProgress);
                        focusTrackOnShelf(prevIdx, g_visibleIndices);
                    }
                }
            }
            bx += secBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - mainBtnSz * 0.5f));
            IconType playIcon = audio.isPlaying() ? IconType::Pause : IconType::Play;
            if (IconButton("##PlayPauseBtn", playIcon, ImVec2(mainBtnSz, mainBtnSz), false, getUICol(bx + mainBtnSz * 0.5f, row1CenterY), hudAlpha, true)) {
                if (g_activePlayingTrack == -1 && !g_playlist.empty()) {
                    playTrackAndFetchLyrics(displayTrackIdx, audio, discFlightProgress);
                } else {
                    audio.togglePlayPause();
                }
            }
            bx += mainBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##NextBtn", IconType::Next, ImVec2(secBtnSz, secBtnSz), false, getUICol(bx + secBtnSz * 0.5f, row1CenterY), hudAlpha)) {
                const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                int nextIdx = getNextTrackIndex(listToUse, true);
                if (nextIdx >= 0) {
                    playTrackAndFetchLyrics(nextIdx, audio, discFlightProgress);
                    focusTrackOnShelf(nextIdx, g_visibleIndices);
                }
            }
            bx += secBtnSz + btnGap;

            LoopMode lm = audio.getLoopMode();
            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            ImVec4 loopCol = getUICol(bx + secBtnSz * 0.5f, row1CenterY);
            if (IconButton("##LoopBtn", IconType::Repeat, ImVec2(secBtnSz, secBtnSz), lm != LoopMode::None, loopCol, hudAlpha)) {
                if (lm == LoopMode::All) audio.setLoopMode(LoopMode::Track);
                else if (lm == LoopMode::Track) audio.setLoopMode(LoopMode::None);
                else audio.setLoopMode(LoopMode::All);
                lastActiveTime = now;
            }
            if (lm == LoopMode::Track) {
                ImVec2 rMin = ImGui::GetItemRectMin();
                dl->AddText(ImVec2(rMin.x + 12.0f, rMin.y + 7.0f), ImColor(loopCol), "1");
            }
            bx += secBtnSz + btnGap;

            // Кнопка «Оборот бокса / Текст песни (Караоке)» в нижнем баре
            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##LyricsFlipBtn", IconType::Lyrics, ImVec2(secBtnSz, secBtnSz), g_lyricsInspectMode, getUICol(bx + secBtnSz * 0.5f, row1CenterY), hudAlpha)) {
                g_lyricsInspectMode = !g_lyricsInspectMode;
                g_inspectShowFront = false;
                if (g_lyricsInspectMode) {
                    triggerAutoLyricsForTrack(displayTrackIdx, audio);
                }
            }

            const float row2CenterY = 61.0f;
            const float barW = 260.0f;
            const float barH = 16.0f;
            const float barX = centerAxisX - barW * 0.5f;

            static bool s_isScrubbing = false;
            static float s_scrubTime = 0.0f;

            float curSec = audio.getPlaybackPosition();
            float totSec = audio.getDuration();
            if (!s_isScrubbing) {
                s_scrubTime = curSec;
            }

            std::string curTimeStr = formatTime(s_isScrubbing ? s_scrubTime : curSec);
            std::string totTimeStr = formatTime(totSec);

            ImVec2 curTimeSz = ImGui::CalcTextSize(curTimeStr.c_str());
            ImVec2 totTimeSz = ImGui::CalcTextSize(totTimeStr.c_str());

            ImGui::SetCursorPos(ImVec2(barX - curTimeSz.x - 10.0f, row2CenterY - curTimeSz.y * 0.5f));
            ImGui::TextColored(getUICol(barX - curTimeSz.x * 0.5f, row2CenterY), "%s", curTimeStr.c_str());

            ImGui::SetCursorPos(ImVec2(barX, row2CenterY - barH * 0.5f));
            if (CustomScrubber("##CenteredScrubber", &s_scrubTime, totSec, barW, barH, getUICol(barX + barW * 0.5f, row2CenterY), true, &s_isScrubbing)) {
                audio.seekTo((totSec > 0.0f) ? (s_scrubTime / totSec) : 0.0f);
            }

            ImGui::SetCursorPos(ImVec2(barX + barW + 10.0f, row2CenterY - totTimeSz.y * 0.5f));
            ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.88f, hudAlpha), "%s", totTimeStr.c_str());

            const float rightCenterY = dockH * 0.5f;
            const float volBarW = 86.0f;
            const float volBarH = 16.0f;
            const float muteBtnSz = 30.0f;
            const float rightPad = 22.0f;

            float volBarX = dockW - rightPad - volBarW;
            float muteBtnX = volBarX - 8.0f - muteBtnSz;

            float curVol = audio.isMuted() ? 0.0f : audio.getVolume();
            IconType volIcon = (audio.isMuted() || curVol <= 0.001f) ? IconType::Mute : IconType::Volume;

            ImGui::SetCursorPos(ImVec2(muteBtnX, rightCenterY - muteBtnSz * 0.5f));
            if (IconButton("##MuteBtn", volIcon, ImVec2(muteBtnSz, muteBtnSz), audio.isMuted(), getUICol(muteBtnX + muteBtnSz * 0.5f, rightCenterY), hudAlpha, false, curVol)) {
                audio.toggleMute();
            }

            ImGui::SetCursorPos(ImVec2(volBarX, rightCenterY - volBarH * 0.5f));
            float vol = curVol;
            if (CustomScrubber("##VolScrubber", &vol, 1.0f, volBarW, volBarH, getUICol(volBarX + volBarW * 0.5f, rightCenterY))) {
                audio.setVolume(vol);
            }

            ImGui::End();
            ImGui::PopStyleColor(3);
        }

        // ------------------ 5. КЛИКИ ПО БОКСАМ И КОНТЕКСТНОЕ МЕНЮ ПО ПКМ ------------------
        if (!io.WantCaptureMouse && !g_roulette.active && mouseY >= 72.0) {
            if (g_lyricsInspectMode) {
                // Клик мимо центрального бокса в режиме осмотра плавно возвращает бокс на полку
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseY < h - 110.0) {
                    glm::vec4 br = renderer.getInspectBoxScreenRect();
                    if (mouseX < br.x - 20.0 || mouseX > br.z + 20.0 || mouseY < br.y - 50.0 || mouseY > br.w + 20.0) {
                        g_lyricsInspectMode = false;
                        lastActiveTime = now;
                    }
                }
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverIndex != -1 && !g_playlist.empty()) {
                int targetIdx = std::clamp(hoverIndex, 0, static_cast<int>(g_playlist.size()) - 1);
                if (g_activePlayingTrack != targetIdx) {
                    playTrackAndFetchLyrics(targetIdx, audio, discFlightProgress);
                } else {
                    audio.togglePlayPause();
                }
                focusTrackOnShelf(targetIdx, g_visibleIndices);
                lastActiveTime = now;
            } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                       glm::length(glm::vec2(io.MouseDelta.x, io.MouseDelta.y)) < 2.0f) {
                g_contextMenuTrack = hoverIndex;
                ImGui::OpenPopup("TrackContextMenu");
                lastActiveTime = now;
            }
        }

        glm::vec3 menuAccent = (g_contextMenuTrack >= 0 && g_contextMenuTrack < static_cast<int>(g_playlist.size()))
            ? g_playlist[g_contextMenuTrack].accentColor
            : currentThemeAccent;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 7.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.09f, 0.13f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.98f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(menuAccent.r, menuAccent.g, menuAccent.b, 0.30f));
        if (ImGui::BeginPopup("TrackContextMenu")) {
            lastActiveTime = now;
            if (g_contextMenuTrack >= 0 && g_contextMenuTrack < static_cast<int>(g_playlist.size())) {
                const auto& ct = g_playlist[g_contextMenuTrack];
                ImGui::TextColored(ImVec4(ct.accentColor.r, ct.accentColor.g, ct.accentColor.b, 1.0f), "%s", truncateUtf8(ct.title, 26).c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Воспроизвести")) {
                    playTrackAndFetchLyrics(g_contextMenuTrack, audio, discFlightProgress);
                    focusTrackOnShelf(g_contextMenuTrack, g_visibleIndices);
                }
                if (ImGui::MenuItem("Текст песни")) {
                    if (g_activePlayingTrack != g_contextMenuTrack) {
                        playTrackAndFetchLyrics(g_contextMenuTrack, audio, discFlightProgress);
                    } else {
                        triggerAutoLyricsForTrack(g_contextMenuTrack, audio);
                    }
                    g_lyricsInspectMode = true;
                    g_inspectShowFront = false;
                    focusTrackOnShelf(g_contextMenuTrack, g_visibleIndices);
                }
                if (ImGui::MenuItem("Показать в проводнике")) {
                    std::wstring param = L"/select,\"" + ct.widePath + L"\"";
                    ShellExecuteW(hwnd, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem(ct.hasRealCover ? "Обновить обложку из сети" : "Найти обложку в сети")) {
                    g_coverFetcher.requestCover(ct.widePath, ct.artist, ct.title, ct.album, true);
                }
                ImGui::Separator();
            }

            if (ImGui::MenuItem("Сортировать по исполнителю")) {
                std::wstring curPlayPath = (g_activePlayingTrack >= 0) ? g_playlist[g_activePlayingTrack].widePath : L"";
                std::sort(g_playlist.begin(), g_playlist.end(), [](const TrackData& a, const TrackData& b) {
                    if (a.artist != b.artist) return a.artist < b.artist;
                    return a.title < b.title;
                });
                g_activePlayingTrack = -1;
                for (size_t i = 0; i < g_playlist.size(); ++i) {
                    if (g_playlist[i].widePath == curPlayPath) {
                        g_activePlayingTrack = static_cast<int>(i);
                        break;
                    }
                }
                g_selectedTrack = g_activePlayingTrack;
                g_searchDirty = true;
                savePlaylistToDisk();
            }

            if (g_contextMenuTrack >= 0 && g_contextMenuTrack < static_cast<int>(g_playlist.size())) {
                ImGui::Separator();
                if (ImGui::MenuItem("Удалить из коллекции")) {
                    if (g_activePlayingTrack == g_contextMenuTrack) {
                        audio.stop();
                        g_activePlayingTrack = -1;
                        g_selectedTrack = -1;
                        g_lyricsInspectMode = false;
                    } else if (g_activePlayingTrack > g_contextMenuTrack) {
                        g_activePlayingTrack--;
                    }
                    if (g_selectedTrack > g_contextMenuTrack) {
                        g_selectedTrack--;
                    }
                    MetadataParser::cleanup(g_playlist[g_contextMenuTrack]);
                    g_playlist.erase(g_playlist.begin() + g_contextMenuTrack);
                    g_hoverWeights.erase(g_hoverWeights.begin() + g_contextMenuTrack);
                    g_searchDirty = true;
                    savePlaylistToDisk();
                    g_contextMenuTrack = -1;
                }
            } else {
                ImGui::Separator();
                if (ImGui::MenuItem("+ Добавить файлы...")) {
                    openFileDialog(hwnd);
                }
                if (ImGui::MenuItem("+ Добавить папку...")) {
                    openFolderDialog(hwnd);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(5);

        // ------------------ ДИАЛОГ С ГОРАЧИМИ КЛАВИШАМИ ------------------
        if (g_showHotkeysDialog) {
            ImGui::OpenPopup("Горячие клавиши##Modal");
        }
        ImGui::SetNextWindowSize(ImVec2(500.0f, 480.0f), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.10f, 0.14f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.18f));
        if (ImGui::BeginPopupModal("Горячие клавиши##Modal", &g_showHotkeysDialog, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextColored(ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 1.0f), "Горячие клавиши Jewel Box Player");
            ImGui::Separator();
            ImGui::Spacing();

            auto drawShortcutRow = [](const char* keys, const char* desc) {
                ImGui::TextColored(ImVec4(0.42f, 0.82f, 1.0f, 1.0f), "%-22s", keys);
                ImGui::SameLine(190.0f);
                ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.98f, 1.0f), "%s", desc);
            };

            drawShortcutRow("Пробел", "Воспроизведение / Пауза");
            drawShortcutRow("Ctrl + ->  /  N  /  .", "Следующий трек");
            drawShortcutRow("Ctrl + <-  /  P  /  ,", "Предыдущий трек");
            drawShortcutRow("<-  /  ->", "Перемотка на ±5 сек (с Shift: ±15 сек)");
            drawShortcutRow("Вверх  /  Вниз", "Громкость ±5%");
            drawShortcutRow("M", "Включить / выключить звук");
            drawShortcutRow("S", "Случайный порядок (Shuffle)");
            drawShortcutRow("Ctrl + R  /  T", "Циклический повтор (Трек / Всё / Выкл)");
            drawShortcutRow("L", "Текст песни (Оборот 3D-бокса)");
            drawShortcutRow("R", "Рулетка случайного выбора");
            drawShortcutRow("Ctrl + F  /  /", "Быстрый поиск");
            drawShortcutRow("Ctrl + O", "Добавить файлы в коллекцию");
            drawShortcutRow("Ctrl + Shift + O", "Добавить папку в коллекцию");
            drawShortcutRow("F3", "Дебаг-монитор производительности");
            drawShortcutRow("Esc", "Закрыть текст / сбросить поиск");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.68f, 0.74f, 0.85f, 0.95f), "Медиаклавиши клавиатуры/гарнитуры и шторка SMTC работают глобально.");

            ImGui::Spacing();
            float btnW = 110.0f;
            ImGui::SetCursorPosX((500.0f - btnW) * 0.5f);
            if (ImGui::Button("Закрыть", ImVec2(btnW, 30.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_showHotkeysDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // ------------------ МОДАЛЬНОЕ ОКНО АВТОРИЗАЦИИ ЯНДЕКС МУЗЫКИ ------------------
        if (g_showYmAuthModal) {
            ImGui::OpenPopup("Яндекс Музыка##AuthModal");
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 440.0f), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.09f, 0.13f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.40f));

        if (ImGui::BeginPopupModal("Яндекс Музыка##AuthModal", &g_showYmAuthModal,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            ImDrawList* mdl = ImGui::GetWindowDrawList();
            ImVec2 mMin = ImGui::GetWindowPos();

            // Кастомный верхний заголовок
            ImVec2 ymIconC(mMin.x + 36.0f, mMin.y + 36.0f);
            mdl->AddCircleFilled(ymIconC, 12.0f, ImColor(0.988f, 0.247f, 0.114f, 1.0f), 24);
            if (g_fontMainBold) {
                const char* letter = "Я";
                ImVec2 tSz = g_fontMainBold->CalcTextSizeA(18.0f, FLT_MAX, -1.0f, letter);
                mdl->AddText(g_fontMainBold, 18.0f, ImVec2(ymIconC.x - tSz.x * 0.5f, ymIconC.y - tSz.y * 0.5f - 1.0f),
                             ImColor(1.0f, 1.0f, 1.0f, 1.0f), letter);
            }

            ImGui::SetCursorPos(ImVec2(56.0f, 18.0f));
            if (g_fontTitleBold) ImGui::PushFont(g_fontTitleBold);
            ImGui::TextColored(ImVec4(0.98f, 0.99f, 1.0f, 1.0f), "Яндекс Музыка");
            if (g_fontTitleBold) ImGui::PopFont();

            // Кнопка закрытия окна (крестик) в правом верхнем углу
            ImGui::SetCursorPos(ImVec2(560.0f - 42.0f, 18.0f));
            if (IconButton("##CloseYmModal", IconType::CloseWin, ImVec2(28.0f, 28.0f), false, ImVec4(1.0f, 1.0f, 1.0f, 0.75f), 1.0f) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_showYmAuthModal = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SetCursorPos(ImVec2(56.0f, 44.0f));
            ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.78f, 1.0f), "Интеграция каталога, обложек и прямого скачивания на 3D-полку");

            ImGui::SetCursorPosY(74.0f);
            ImGui::Separator();
            ImGui::Spacing();

            bool isAuth = YandexMusicManager::instance().isAuthenticated();
            std::string uName = YandexMusicManager::instance().getUserName();

            if (isAuth) {
                // Карточка подключённого профиля
                ImVec2 cardP = ImGui::GetCursorScreenPos();
                float cardW = ImGui::GetContentRegionAvail().x;
                float cardH = 150.0f;

                mdl->AddRectFilled(cardP, ImVec2(cardP.x + cardW, cardP.y + cardH),
                                  ImColor(0.10f, 0.22f, 0.16f, 0.40f), 14.0f);
                mdl->AddRect(cardP, ImVec2(cardP.x + cardW, cardP.y + cardH),
                             ImColor(0.20f, 0.85f, 0.45f, 0.40f), 14.0f, 0, 1.2f);

                // Зелёная статусная точка с галочкой
                ImVec2 statBadgeC(cardP.x + 28.0f, cardP.y + 32.0f);
                mdl->AddCircleFilled(statBadgeC, 10.0f, ImColor(0.18f, 0.82f, 0.42f, 1.0f), 20);
                mdl->AddLine(ImVec2(statBadgeC.x - 4.0f, statBadgeC.y + 0.5f),
                             ImVec2(statBadgeC.x - 1.0f, statBadgeC.y + 3.5f), ImColor(1.0f, 1.0f, 1.0f, 1.0f), 1.8f);
                mdl->AddLine(ImVec2(statBadgeC.x - 1.0f, statBadgeC.y + 3.5f),
                             ImVec2(statBadgeC.x + 4.5f, statBadgeC.y - 3.0f), ImColor(1.0f, 1.0f, 1.0f, 1.0f), 1.8f);

                mdl->AddText(ImVec2(cardP.x + 48.0f, cardP.y + 24.0f),
                             ImColor(0.35f, 0.92f, 0.50f, 1.0f), "Аккаунт успешно подключён!");

                std::string userDisplay = "Пользователь: " + (uName.empty() ? std::string("Активен") : uName);
                if (g_fontMainBold) {
                    mdl->AddText(g_fontMainBold, 17.0f, ImVec2(cardP.x + 22.0f, cardP.y + 56.0f),
                                 ImColor(0.96f, 0.98f, 1.0f, 1.0f), userDisplay.c_str());
                } else {
                    mdl->AddText(ImVec2(cardP.x + 22.0f, cardP.y + 56.0f),
                                 ImColor(0.96f, 0.98f, 1.0f, 1.0f), userDisplay.c_str());
                }

                // Информационные бейджи преимуществ
                float bX = cardP.x + 22.0f;
                float bY = cardP.y + 95.0f;
                const char* tags[] = { "MP3 320 kbps", "HD Обложки", "Авто-кэш", "Тексты песен" };
                for (const char* tg : tags) {
                    ImVec2 tSz = ImGui::CalcTextSize(tg);
                    mdl->AddRectFilled(ImVec2(bX, bY), ImVec2(bX + tSz.x + 16.0f, bY + 26.0f),
                                       ImColor(1.0f, 1.0f, 1.0f, 0.08f), 13.0f);
                    mdl->AddRect(ImVec2(bX, bY), ImVec2(bX + tSz.x + 16.0f, bY + 26.0f),
                                 ImColor(1.0f, 1.0f, 1.0f, 0.12f), 13.0f, 0, 1.0f);
                    mdl->AddText(ImVec2(bX + 8.0f, bY + (26.0f - tSz.y) * 0.5f),
                                 ImColor(0.80f, 0.85f, 0.95f, 1.0f), tg);
                    bX += tSz.x + 24.0f;
                }

                ImGui::Dummy(ImVec2(0, cardH + 16.0f));

                ImGui::TextColored(ImVec4(0.68f, 0.74f, 0.85f, 1.0f),
                                   "Вам доступен полный каталог Яндекс Музыки прямо в поисковой строке плеера.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Кнопки выхода и закрытия
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.22f, 0.25f, 0.20f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.25f, 0.30f, 0.50f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.70f, 1.0f));
                if (ImGui::Button("Выйти из аккаунта", ImVec2(160.0f, 34.0f))) {
                    YandexMusicManager::instance().logoutAsync();
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 144.0f);
                if (ImGui::Button("Готово", ImVec2(120.0f, 34.0f))) {
                    g_showYmAuthModal = false;
                    ImGui::CloseCurrentPopup();
                }
            } else {
                // Шаг 1: Авторизация в браузере
                ImVec2 card1P = ImGui::GetCursorScreenPos();
                float cardW = ImGui::GetContentRegionAvail().x;
                float card1H = 92.0f;

                mdl->AddRectFilled(card1P, ImVec2(card1P.x + cardW, card1P.y + card1H),
                                  ImColor(1.0f, 1.0f, 1.0f, 0.04f), 12.0f);
                mdl->AddRect(card1P, ImVec2(card1P.x + cardW, card1P.y + card1H),
                             ImColor(1.0f, 1.0f, 1.0f, 0.10f), 12.0f, 0, 1.0f);

                // Номер шага 1
                ImVec2 b1C(card1P.x + 24.0f, card1P.y + 24.0f);
                mdl->AddCircleFilled(b1C, 10.0f, ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.40f), 20);
                mdl->AddText(ImVec2(b1C.x - 3.5f, b1C.y - 7.0f), ImColor(1.0f, 1.0f, 1.0f, 1.0f), "1");

                mdl->AddText(ImVec2(card1P.x + 42.0f, card1P.y + 16.0f),
                             ImColor(0.95f, 0.98f, 1.0f, 1.0f), "Шаг 1: Авторизуйтесь в Яндексе через браузер");

                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 42.0f, ImGui::GetCursorPosY() + 42.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.988f, 0.247f, 0.114f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.35f, 0.20f, 1.0f));
                if (ImGui::Button("Открыть страницу входа Яндекс", ImVec2(cardW - 60.0f, 32.0f))) {
                    ShellExecuteW(nullptr, L"open",
                        L"https://oauth.yandex.ru/authorize?response_type=token&client_id=23cabbbdc6cd418abb4b39c32c41195d",
                        nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor(2);

                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 26.0f);

                // Шаг 2: Ввод ссылки / токена
                ImVec2 card2P = ImGui::GetCursorScreenPos();
                float card2H = 110.0f;

                mdl->AddRectFilled(card2P, ImVec2(card2P.x + cardW, card2P.y + card2H),
                                  ImColor(1.0f, 1.0f, 1.0f, 0.04f), 12.0f);
                mdl->AddRect(card2P, ImVec2(card2P.x + cardW, card2P.y + card2H),
                             ImColor(1.0f, 1.0f, 1.0f, 0.10f), 12.0f, 0, 1.0f);

                // Номер шага 2
                ImVec2 b2C(card2P.x + 24.0f, card2P.y + 24.0f);
                mdl->AddCircleFilled(b2C, 10.0f, ImColor(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.40f), 20);
                mdl->AddText(ImVec2(b2C.x - 3.5f, b2C.y - 7.0f), ImColor(1.0f, 1.0f, 1.0f, 1.0f), "2");

                mdl->AddText(ImVec2(card2P.x + 42.0f, card2P.y + 16.0f),
                             ImColor(0.95f, 0.98f, 1.0f, 1.0f), "Шаг 2: Вставьте URL или токен из адресной строки");

                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 42.0f, ImGui::GetCursorPosY() + 42.0f));
                ImGui::SetNextItemWidth(cardW - 60.0f);
                ImGui::InputTextWithHint("##YmTokenIn", "Вставьте https://music.yandex.ru/#access_token=... или токен",
                                         g_ymTokenInput, sizeof(g_ymTokenInput));

                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 38.0f);

                // Статусное сообщение
                std::string authMsg = YandexMusicManager::instance().getAuthMessage();
                bool busy = YandexMusicManager::instance().isAuthBusy();
                if (busy) {
                    ImGui::TextColored(ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 1.0f), "Проверка токена в сервисе Яндекс Музыки...");
                } else if (!authMsg.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s", authMsg.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.72f, 1.0f), "Токен хранится локально в файле ym_token.txt на вашем ПК.");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Кнопки сохранения и отмены
                bool canSubmit = (g_ymTokenInput[0] != '\0') && !busy;
                if (!canSubmit) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                }
                if (ImGui::Button("Сохранить и подключить", ImVec2(200.0f, 34.0f)) && canSubmit) {
                    YandexMusicManager::instance().submitTokenAsync(g_ymTokenInput, [](bool success, const std::string&) {
                        if (success) {
                            g_ymTokenInput[0] = '\0';
                        }
                    });
                }
                if (!canSubmit) {
                    ImGui::PopStyleColor(2);
                }

                ImGui::SameLine();
                if (ImGui::Button("Отмена", ImVec2(110.0f, 34.0f))) {
                    g_showYmAuthModal = false;
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        bool isActivelyAnimating = (now - lastActiveTime < 0.75) ||
                                   (now - g_lastScrollTime < 0.75) ||
                                   renderer.isAccentTransitioning() ||
                                   isScrolling ||
                                   g_roulette.active ||
                                   (g_settingsAnimProgress > 0.001f && g_settingsAnimProgress < 0.999f) ||
                                   (std::abs(targetInspect - g_inspectProgress) > 0.001f) ||
                                   (std::abs(targetFrontFlip - g_inspectFrontFlip) > 0.001f) ||
                                   (discFlightProgress < 1.0f && g_selectedTrack != -1) ||
                                   !g_pendingLoadQueue.empty();

        // ------------------ ОТОБРАЖЕНИЕ ДЕБАГ-МОНИТОРА ТЕЛЕМЕТРИИ (F3) ------------------
        renderDebugOverlay(isActivelyAnimating, currentThemeAccent, w, h, audio);

        syncSmtcPlaybackState(audio);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (gpuQueryActive) {
            glEndQuery(GL_TIME_ELAPSED);
            g_gpuQueryWriteIdx = (g_gpuQueryWriteIdx + 1) % kGpuQueryCount;
        }

        glfwSwapBuffers(window);

        // Периодический расчет метрик для дебаг-монитора (каждые ~0.45 сек, точно как частота обновления Диспетчера задач)
        g_telemetryFrameCounter++;
        double telemetryDt = now - g_lastTelemetrySampleTime;
        if (telemetryDt >= 0.45 && g_telemetryFrameCounter > 0) {
            // 1. Кадровая частота
            g_telemetryFps = static_cast<float>(g_telemetryFrameCounter / telemetryDt);
            g_telemetryFrameMs = static_cast<float>((telemetryDt / g_telemetryFrameCounter) * 1000.0);
            g_telemetryFrameCounter = 0;

            // 2. Нагрузка ГПУ (процент времени, когда 3D кремний GPU выполнял команды процесса)
            double intervalNs = telemetryDt * 1.0e9;
            if (intervalNs > 0.0) {
                double gpuPct = (static_cast<double>(g_accumGpuTimeNs) / intervalNs) * 100.0;
                g_telemetryGpuPercent = static_cast<float>(std::clamp(gpuPct, 0.0, 100.0));
            }
            g_accumGpuTimeNs = 0;

            // 3. Нагрузка ЦПУ (официальная формула Windows Диспетчера задач: deltaProc / (deltaWall * cores))
            FILETIME ftCreation, ftExit, ftKernel, ftUser, ftNow;
            if (GetProcessTimes(GetCurrentProcess(), &ftCreation, &ftExit, &ftKernel, &ftUser)) {
                GetSystemTimeAsFileTime(&ftNow);
                uint64_t curProcTime = fileTimeToUint64(ftKernel) + fileTimeToUint64(ftUser);
                uint64_t curWallTime = fileTimeToUint64(ftNow);

                if (g_prevWallTime > 0 && curWallTime > g_prevWallTime) {
                    uint64_t deltaProc = curProcTime - g_prevProcTime;
                    uint64_t deltaWall = curWallTime - g_prevWallTime;
                    if (deltaWall > 0 && g_numLogicalCores > 0) {
                        double cpuPct = (static_cast<double>(deltaProc) / static_cast<double>(deltaWall * g_numLogicalCores)) * 100.0;
                        g_telemetryCpuPercent = static_cast<float>(std::clamp(cpuPct, 0.0, 100.0));
                    }
                }
                g_prevProcTime = curProcTime;
                g_prevWallTime = curWallTime;
            }

            // 4. Память (Working Set / Рабочий набор процесса в ОЗУ)
            PROCESS_MEMORY_COUNTERS_EX pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                g_telemetryRamMb = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
            }

            g_lastTelemetrySampleTime = now;
        }

        // Адаптивное управление частотой кадров (высокоточный энергоэффективный пейсинг):
        // 1) В активном режиме (скролл, анимации, рулетка) ограничиваем кадры ровно 120 FPS
        //    (при отключённом VSync в драйвере GPU видеокарта не молотит на 1500+ FPS, экономя ресурс и энергию).
        // 2) В покое (статика, прослушивание музыки) усыпляем поток до 30 FPS (или 60 FPS при открытом тексте/настройках)
        //    для ультра-низкого потребления 0.1% CPU и 0.5% GPU.
        double targetFps = isActivelyAnimating
            ? 120.0
            : ((g_lyricsInspectMode || g_showSettingsWindow) ? 60.0 : 30.0);

        double targetFrameTime = 1.0 / targetFps;
        double elapsed = glfwGetTime() - frameStart;
        if (elapsed < targetFrameTime) {
            DWORD sleepMs = static_cast<DWORD>((targetFrameTime - elapsed) * 1000.0);
            if (sleepMs > 0) {
                Sleep(sleepMs);
            }
        }
    }

    if (g_gpuQueriesInitialized && glDeleteQueries != nullptr) {
        glDeleteQueries(kGpuQueryCount, g_gpuQueries);
        g_gpuQueriesInitialized = false;
    }

    saveSettingsToDisk(audio);

    for (auto& track : g_playlist) {
        MetadataParser::cleanup(track);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    SmtcManager::instance().shutdown();
    YandexMusicManager::instance().shutdown();

    glfwTerminate();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return main();
}
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main();
}
#endif