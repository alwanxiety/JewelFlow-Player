#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>

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
#include <algorithm>
#include <random>
#include <iomanip>
#include <sstream>
#include <cmath>

#include "AudioEngine.hpp"
#include "Metadata.hpp"
#include "Renderer.hpp"

namespace fs = std::filesystem;

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
static WNDPROC g_origWndProc = nullptr;

static std::mt19937& getRng() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

// Кастомная оконная процедура: убирает стандартную белую шапку через WM_NCCALCSIZE,
// но сохраняет стили WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
// благодаря чему работают ВСЕ нативные плавные анимации Windows 11 (сворачивание, разворачивание, полный экран, Aero Snap)!
static LRESULT CALLBACK CustomBorderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

    // Включаем нативные флаги окна, необходимые DWM для плавных анимаций сворачивания и разворачивания
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= (WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

    COLORREF borderCol = RGB(36, 40, 52);
    DwmSetWindowAttribute(hwnd, 34, &borderCol, sizeof(borderCol));

    BOOL transitionsDisabled = FALSE; // Гарантируем, что DWM-анимации включены для окна
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
            }
        } catch (...) {}
    }
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
    if (!g_searchDirty && g_lastSearchQuery == g_searchQuery) return;
    g_lastSearchQuery = g_searchQuery;
    g_searchDirty = false;

    g_visibleIndices.clear();
    std::wstring wQuery = MetadataParser::toLowerWide(MetadataParser::utf8ToWide(g_searchQuery));

    for (size_t i = 0; i < g_playlist.size(); ++i) {
        if (wQuery.empty() || g_playlist[i].searchKeyLower.find(wQuery) != std::wstring::npos) {
            g_visibleIndices.push_back(static_cast<int>(i));
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
    if (!ImGui::GetIO().WantCaptureMouse && !g_roulette.active) {
        float delta = (std::abs(xoffset) > std::abs(yoffset)) ? static_cast<float>(xoffset) : static_cast<float>(yoffset);
        g_targetScroll -= delta * 0.34f;
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
    int count = static_cast<int>(visibleIndices.size());
    if (count == 1) {
        int onlyIdx = visibleIndices[0];
        g_selectedTrack = onlyIdx;
        g_activePlayingTrack = onlyIdx;
        discFlightProgress = 0.0f;
        audio.playFile(g_playlist[onlyIdx].widePath);
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

static bool CustomScrubber(const char* label, float* value, float max_val, float width, float height, const ImVec4& accentColor) {
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

    return held;
}

enum class IconType { Play, Pause, Prev, Next, Shuffle, Repeat, Volume, Mute, Dice, Minimize, Maximize, Restore, CloseWin };

static bool IconButton(const char* idStr, IconType icon, ImVec2 size, bool activeState, const ImVec4& accent, float alpha, bool primary = false) {
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

    bool isCloseBtn = (icon == IconType::CloseWin);

    if (primary) {
        ImVec4 bg = held ? ImVec4(0.88f, 0.90f, 0.95f, alpha)
                  : (hovered ? ImVec4(1.0f, 1.0f, 1.0f, alpha) : ImVec4(0.95f, 0.96f, 0.98f, 0.95f * alpha));
        dl->AddCircleFilled(c, size.y * 0.5f, ImColor(bg), 32);
    } else if (hovered || held || activeState) {
        if (isCloseBtn && (hovered || held)) {
            ImVec4 bg = held ? ImVec4(0.82f, 0.18f, 0.24f, 0.95f * alpha)
                             : ImVec4(0.92f, 0.24f, 0.30f, 0.88f * alpha);
            dl->AddRectFilled(bb.Min, bb.Max, ImColor(bg), size.y * 0.42f);
        } else {
            float bgAlpha = held ? 0.28f : (hovered ? 0.18f : 0.12f);
            ImVec4 bg = activeState ? ImVec4(accent.x, accent.y, accent.z, bgAlpha * alpha * 1.4f)
                                    : ImVec4(1.0f, 1.0f, 1.0f, bgAlpha * alpha);
            dl->AddRectFilled(bb.Min, bb.Max, ImColor(bg), size.y * 0.42f);
        }
    }

    ImU32 fg = primary
        ? ImColor(ImVec4(0.07f, 0.08f, 0.11f, alpha))
        : (activeState ? ImColor(ImVec4(accent.x, accent.y, accent.z, alpha))
                       : ImColor(ImVec4(0.92f, 0.94f, 0.98f, (hovered ? 1.0f : 0.80f) * alpha)));

    switch (icon) {
        case IconType::Play: {
            dl->AddTriangleFilled(
                ImVec2(c.x - 3.5f, c.y - 6.5f),
                ImVec2(c.x - 3.5f, c.y + 6.5f),
                ImVec2(c.x + 6.5f, c.y),
                fg
            );
            break;
        }
        case IconType::Pause: {
            dl->AddRectFilled(ImVec2(c.x - 5.5f, c.y - 6.0f), ImVec2(c.x - 2.0f, c.y + 6.0f), fg, 1.2f);
            dl->AddRectFilled(ImVec2(c.x + 2.0f, c.y - 6.0f), ImVec2(c.x + 5.5f, c.y + 6.0f), fg, 1.2f);
            break;
        }
        case IconType::Prev: {
            dl->AddRectFilled(ImVec2(c.x - 6.5f, c.y - 5.5f), ImVec2(c.x - 4.5f, c.y + 5.5f), fg, 1.0f);
            dl->AddTriangleFilled(
                ImVec2(c.x + 5.5f, c.y - 5.5f),
                ImVec2(c.x + 5.5f, c.y + 5.5f),
                ImVec2(c.x - 3.5f, c.y),
                fg
            );
            break;
        }
        case IconType::Next: {
            dl->AddTriangleFilled(
                ImVec2(c.x - 5.5f, c.y - 5.5f),
                ImVec2(c.x - 5.5f, c.y + 5.5f),
                ImVec2(c.x + 3.5f, c.y),
                fg
            );
            dl->AddRectFilled(ImVec2(c.x + 4.5f, c.y - 5.5f), ImVec2(c.x + 6.5f, c.y + 5.5f), fg, 1.0f);
            break;
        }
        case IconType::Shuffle: {
            dl->AddLine(ImVec2(c.x - 6.0f, c.y - 4.0f), ImVec2(c.x + 5.0f, c.y + 4.0f), fg, 1.8f);
            dl->AddLine(ImVec2(c.x - 6.0f, c.y + 4.0f), ImVec2(c.x + 5.0f, c.y - 4.0f), fg, 1.8f);
            dl->AddTriangleFilled(ImVec2(c.x + 3.0f, c.y - 6.5f), ImVec2(c.x + 7.0f, c.y - 4.0f), ImVec2(c.x + 3.0f, c.y - 1.5f), fg);
            dl->AddTriangleFilled(ImVec2(c.x + 3.0f, c.y + 1.5f), ImVec2(c.x + 7.0f, c.y + 4.0f), ImVec2(c.x + 3.0f, c.y + 6.5f), fg);
            break;
        }
        case IconType::Repeat: {
            dl->AddRect(ImVec2(c.x - 6.5f, c.y - 4.5f), ImVec2(c.x + 6.5f, c.y + 4.5f), fg, 4.0f, 0, 1.8f);
            break;
        }
        case IconType::Volume: {
            dl->AddTriangleFilled(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x - 1.0f, c.y - 5.0f), ImVec2(c.x - 1.0f, c.y + 5.0f), fg);
            dl->AddRectFilled(ImVec2(c.x - 6.5f, c.y - 2.5f), ImVec2(c.x - 3.0f, c.y + 2.5f), fg);
            dl->AddBezierQuadratic(ImVec2(c.x + 2.0f, c.y - 4.0f), ImVec2(c.x + 5.5f, c.y), ImVec2(c.x + 2.0f, c.y + 4.0f), fg, 1.6f);
            break;
        }
        case IconType::Mute: {
            dl->AddTriangleFilled(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x - 1.0f, c.y - 5.0f), ImVec2(c.x - 1.0f, c.y + 5.0f), fg);
            dl->AddRectFilled(ImVec2(c.x - 6.5f, c.y - 2.5f), ImVec2(c.x - 3.0f, c.y + 2.5f), fg);
            dl->AddLine(ImVec2(c.x + 1.5f, c.y - 3.5f), ImVec2(c.x + 6.5f, c.y + 3.5f), fg, 1.8f);
            dl->AddLine(ImVec2(c.x + 6.5f, c.y - 3.5f), ImVec2(c.x + 1.5f, c.y + 3.5f), fg, 1.8f);
            break;
        }
        case IconType::Dice: {
            dl->AddRect(ImVec2(c.x - 7.0f, c.y - 7.0f), ImVec2(c.x + 7.0f, c.y + 7.0f), fg, 3.0f, 0, 1.8f);
            dl->AddCircleFilled(ImVec2(c.x - 3.2f, c.y - 3.2f), 1.35f, fg);
            dl->AddCircleFilled(ImVec2(c.x + 3.2f, c.y - 3.2f), 1.35f, fg);
            dl->AddCircleFilled(ImVec2(c.x,        c.y),        1.35f, fg);
            dl->AddCircleFilled(ImVec2(c.x - 3.2f, c.y + 3.2f), 1.35f, fg);
            dl->AddCircleFilled(ImVec2(c.x + 3.2f, c.y + 3.2f), 1.35f, fg);
            break;
        }
        case IconType::Minimize: {
            dl->AddLine(ImVec2(c.x - 5.5f, c.y + 1.0f), ImVec2(c.x + 5.5f, c.y + 1.0f), fg, 1.8f);
            break;
        }
        case IconType::Maximize: {
            dl->AddRect(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), fg, 1.5f, 0, 1.6f);
            break;
        }
        case IconType::Restore: {
            dl->AddRect(ImVec2(c.x - 3.0f, c.y - 5.5f), ImVec2(c.x + 5.5f, c.y + 3.0f), fg, 1.2f, 0, 1.5f);
            dl->AddRect(ImVec2(c.x - 5.5f, c.y - 3.0f), ImVec2(c.x + 3.0f, c.y + 5.5f), fg, 1.2f, 0, 1.5f);
            break;
        }
        case IconType::CloseWin: {
            dl->AddLine(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), fg, 1.8f);
            dl->AddLine(ImVec2(c.x + 5.0f, c.y - 5.0f), ImVec2(c.x - 5.0f, c.y + 5.0f), fg, 1.8f);
            break;
        }
    }

    return pressed;
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);

    if (!glfwInit()) {
        timeEndPeriod(1);
        return -1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
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

    if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(primary)) {
            glfwSetWindowPos(window, (mode->width - 1380) / 2, (mode->height - 800) / 2);
        }
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, dropCallback);
    glfwSetScrollCallback(window, scrollCallback);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

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
        0x2010, 0x205E,
        0x2116, 0x2116,
        0
    };
    builder.AddRanges(extraPunctuation);
    builder.BuildRanges(&glyphRanges);

    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (fs::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr, glyphRanges.Data);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 22.0f;
    style.FrameRounding = 12.0f;
    style.GrabRounding = 8.0f;
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

        if (IsIconic(hwnd) || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.15);
            lastTime = glfwGetTime();
            continue;
        }

        glfwPollEvents();

        double now = glfwGetTime();
        float dt = std::clamp(static_cast<float>(now - lastTime), 0.0001f, 0.05f);
        lastTime = now;

        if (!g_pendingLoadQueue.empty()) {
            lastActiveTime = now;
            for (int k = 0; k < 2 && !g_pendingLoadQueue.empty(); ++k) {
                std::wstring nextPath = g_pendingLoadQueue.front();
                g_pendingLoadQueue.pop_front();
                addTrackImmediate(nextPath);
            }
            if (g_pendingLoadQueue.empty() && g_pendingRestoreTrack >= 0 &&
                g_pendingRestoreTrack < static_cast<int>(g_playlist.size()) && g_activePlayingTrack == -1) {
                g_activePlayingTrack = g_pendingRestoreTrack;
                audio.playFile(g_playlist[g_activePlayingTrack].widePath, true);
                g_pendingRestoreTrack = -1;
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
                            g_selectedTrack = winTrack;
                            g_activePlayingTrack = winTrack;
                            discFlightProgress = 0.0f;
                            audio.playFile(g_playlist[winTrack].widePath);
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

        // Скролл полки мышью и плавный автоскролл у краёв
        if (!io.WantCaptureMouse && !g_visibleIndices.empty() && !g_roulette.active) {
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
            g_targetScroll = std::clamp(g_targetScroll, -0.35f, maxScroll + 0.35f);
            float diffScroll = g_targetScroll - g_shelfScroll;
            if (std::abs(diffScroll) > 0.0002f) {
                g_shelfScroll += diffScroll * std::min(1.0f, 12.0f * dt);
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
        } else if (io.WantCaptureMouse || g_visibleIndices.empty() || mouseY < 75.0) {
            hoverIndex = -1;
            cachedPickIndex = -1;
        } else if (mouseMoved || isScrolling || !g_pendingLoadQueue.empty()) {
            cachedPickIndex = renderer.pickTrack(mouseX, mouseY, g_visibleIndices, g_hoverWeights, g_shelfScroll);
            hoverIndex = cachedPickIndex;
        } else {
            hoverIndex = cachedPickIndex;
        }

        for (size_t i = 0; i < g_hoverWeights.size(); ++i) {
            float targetW = (static_cast<int>(i) == hoverIndex) ? 1.0f : 0.0f;
            float diffW = targetW - g_hoverWeights[i];
            if (std::abs(diffW) > 0.001f) {
                float speed = g_roulette.active ? 18.0f : 13.0f;
                g_hoverWeights[i] += diffW * std::min(1.0f, speed * dt);
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

        if (audio.isEnded() && g_activePlayingTrack != -1) {
            LoopMode lm = audio.getLoopMode();
            if (lm == LoopMode::Track) {
                audio.playFile(g_playlist[g_activePlayingTrack].widePath);
                discFlightProgress = 0.0f;
            } else {
                bool allowWrap = (lm == LoopMode::All);
                const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{g_activePlayingTrack} : g_visibleIndices;
                int nextIdx = getNextTrackIndex(listToUse, allowWrap);
                if (nextIdx >= 0 && nextIdx < static_cast<int>(g_playlist.size())) {
                    g_selectedTrack = nextIdx;
                    g_activePlayingTrack = nextIdx;
                    discFlightProgress = 0.0f;
                    audio.playFile(g_playlist[nextIdx].widePath);
                    focusTrackOnShelf(nextIdx, g_visibleIndices);
                } else {
                    audio.stop();
                }
            }
            lastActiveTime = now;
        }

        int accentSourceIdx = g_roulette.active ? g_roulette.currentTrack : g_activePlayingTrack;
        glm::vec3 targetAccent = (accentSourceIdx >= 0 && accentSourceIdx < static_cast<int>(g_playlist.size()))
            ? g_playlist[accentSourceIdx].accentColor
            : glm::vec3(0.95f, 0.26f, 0.42f);
        currentThemeAccent += (targetAccent - currentThemeAccent) * std::min(1.0f, 6.0f * dt);

        float targetHud = (!g_playlist.empty()) ? 1.0f : 0.0f;
        hudAlpha += (targetHud - hudAlpha) * std::min(1.0f, 9.0f * dt);

        int rouletteHi = g_roulette.active ? g_roulette.currentTrack : -1;
        renderer.renderShelf(g_playlist, g_visibleIndices, g_hoverWeights, g_shelfScroll,
                             g_selectedTrack, g_activePlayingTrack, rouletteHi,
                             discFlightProgress, static_cast<float>(now), currentThemeAccent);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                if (g_activePlayingTrack != -1) {
                    audio.togglePlayPause();
                } else if (!g_visibleIndices.empty()) {
                    int firstIdx = g_visibleIndices[0];
                    g_selectedTrack = firstIdx;
                    g_activePlayingTrack = firstIdx;
                    discFlightProgress = 0.0f;
                    audio.playFile(g_playlist[firstIdx].widePath);
                }
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R) && !g_visibleIndices.empty()) {
                startRandomRoulette(g_visibleIndices, audio, discFlightProgress);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                audio.seekBySeconds(-5.0f);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                audio.seekBySeconds(5.0f);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                audio.setVolume(audio.getVolume() + 0.05f);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                audio.setVolume(audio.getVolume() - 0.05f);
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_M)) {
                audio.toggleMute();
                lastActiveTime = now;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_searchQuery[0] != '\0') {
                g_searchQuery[0] = '\0';
                lastActiveTime = now;
            }
        }

        // ------------------ 1. ВСПЛЫВАЮЩЕЕ ОКНО ПРЯМО ПОД БОКСОМ + КУРСОР РУЛЕТКИ ------------------
        int bestHoverSlot = -1;
        float bestHoverWeight = 0.04f;
        for (size_t slot = 0; slot < g_visibleIndices.size(); ++slot) {
            int rIdx = g_visibleIndices[slot];
            if (rIdx >= 0 && rIdx < static_cast<int>(g_hoverWeights.size())) {
                if (g_hoverWeights[rIdx] > bestHoverWeight) {
                    bestHoverWeight = g_hoverWeights[rIdx];
                    bestHoverSlot = static_cast<int>(slot);
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

        // ------------------ 2. ВЕРХНЯЯ ОБЛАСТЬ: ЦЕНТРАЛЬНЫЙ ПОИСК + СТИЛИЗОВАННЫЕ КНОПКИ ОКНА СПРАВА ------------------
        const float topBarY = 16.0f;
        const float topBarH = 46.0f;
        const float itemH = 30.0f;
        const float itemY = (topBarH - itemH) * 0.5f;

        // 2.1. Центральная панель импорта и поиска
        const float topBarW = 630.0f;
        ImGui::SetNextWindowPos(ImVec2((w - topBarW) * 0.5f, topBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(topBarW, topBarH), ImGuiCond_Always);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.14f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.99f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.52f, 0.56f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(currentThemeAccent.r * 0.8f, currentThemeAccent.g * 0.8f, currentThemeAccent.b * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));

        float fontH = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, (itemH - fontH) * 0.5f));

        ImGui::Begin("TopBarClean", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        float curX = 10.0f;
        ImGui::SetCursorPos(ImVec2(curX, itemY));
        if (ImGui::Button("+ Файлы", ImVec2(88.0f, itemH))) {
            openFileDialog(hwnd);
        }
        curX += 88.0f + 8.0f;

        ImGui::SetCursorPos(ImVec2(curX, itemY));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
        if (ImGui::Button("+ Папка", ImVec2(84.0f, itemH))) {
            openFolderDialog(hwnd);
        }
        ImGui::PopStyleColor(2);
        curX += 84.0f + 10.0f;

        std::ostringstream countSS;
        countSS << g_visibleIndices.size() << " / " << (g_playlist.size() + g_pendingLoadQueue.size());
        std::string countStr = countSS.str();
        ImVec2 countSz = ImGui::CalcTextSize(countStr.c_str());
        float rightReserved = countSz.x + 28.0f;
        bool hasClearBtn = (g_searchQuery[0] != '\0');
        float searchW = topBarW - curX - rightReserved - (hasClearBtn ? 36.0f : 0.0f);

        ImGui::SetCursorPos(ImVec2(curX, itemY));
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##SearchInput", "Поиск по названию или исполнителю...", g_searchQuery, sizeof(g_searchQuery));
        curX += searchW + 6.0f;

        if (hasClearBtn) {
            ImGui::SetCursorPos(ImVec2(curX, itemY));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.24f));
            if (ImGui::Button("X##ClearSearch", ImVec2(30.0f, itemH))) {
                g_searchQuery[0] = '\0';
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::SetCursorPos(ImVec2(topBarW - countSz.x - 14.0f, (topBarH - countSz.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.0f), "%s", countStr.c_str());

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(8);

        // 2.2. Правая капсула управления окном (вызывает нативные системные команды SC_MINIMIZE / SC_MAXIMIZE / SC_RESTORE с плавными анимациями Windows!)
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
            ImVec4 accCol(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, 1.0f);

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

        // Нативное перетаскивание окна за свободную верхнюю область (и двойной клик с нативной анимацией SC_MAXIMIZE / SC_RESTORE)
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

        // ================== 3. НИЖНИЙ БАР ПЛЕЕРА ==================
        if (hudAlpha > 0.01f && !g_playlist.empty()) {
            const float dockW = std::min(860.0f, static_cast<float>(w) - 32.0f);
            const float dockH = 84.0f;
            const float slideY = (1.0f - hudAlpha) * 45.0f;
            const float dockX = (w - dockW) * 0.5f;
            const float dockY = static_cast<float>(h) - dockH - 18.0f + slideY;

            ImVec4 barBgColor(
                currentThemeAccent.r * 0.14f + 0.06f,
                currentThemeAccent.g * 0.14f + 0.07f,
                currentThemeAccent.b * 0.14f + 0.09f,
                0.92f * hudAlpha
            );
            ImVec4 barBorderColor(
                currentThemeAccent.r * 0.55f + 0.30f,
                currentThemeAccent.g * 0.55f + 0.30f,
                currentThemeAccent.b * 0.55f + 0.30f,
                0.35f * hudAlpha
            );
            ImVec4 accentRGBA(currentThemeAccent.r, currentThemeAccent.g, currentThemeAccent.b, hudAlpha);

            ImGui::SetNextWindowPos(ImVec2(dockX, dockY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(dockW, dockH), ImGuiCond_Always);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, barBgColor);
            ImGui::PushStyleColor(ImGuiCol_Border, barBorderColor);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, hudAlpha));

            ImGui::Begin("LiquidGlassDockBar", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

            int displayTrackIdx = (g_activePlayingTrack >= 0 && g_activePlayingTrack < static_cast<int>(g_playlist.size()))
                ? g_activePlayingTrack
                : 0;
            const auto& curTrack = g_playlist[displayTrackIdx];
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 winPos = ImGui::GetWindowPos();

            const float coverSz = 56.0f;
            const float coverX = 16.0f;
            const float coverY = (dockH - coverSz) * 0.5f;

            ImGui::SetCursorPos(ImVec2(coverX, coverY));
            ImGui::InvisibleButton("##RoundedCoverBtn", ImVec2(coverSz, coverSz));
            if (ImGui::IsItemClicked()) {
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
                dl->AddRectFilled(ImVec2(bx, eqBase.y - bh), ImVec2(bx + 2.8f, eqBase.y), ImColor(accentRGBA), 1.2f);
            }

            std::string safeTitle = truncateUtf8(curTrack.title, 20);
            std::string subLine = truncateUtf8(curTrack.artist, 20) + "  |  " + curTrack.formatBadge;

            ImGui::SetCursorPos(ImVec2(textX + eqWidth + 4.0f, textStartY));
            ImGui::TextColored(ImVec4(0.98f, 0.99f, 1.0f, hudAlpha), "%s", safeTitle.c_str());

            ImGui::SetCursorPos(ImVec2(textX, textStartY + lineH + 4.0f));
            ImGui::TextColored(ImVec4(0.70f, 0.76f, 0.86f, hudAlpha), "%s", subLine.c_str());

            const float centerAxisX = dockW * 0.5f;
            const float row1CenterY = 26.0f;
            const float secBtnSz = 30.0f;
            const float mainBtnSz = 36.0f;
            const float btnGap = 9.0f;
            const float totalBtnsW = secBtnSz * 5.0f + mainBtnSz + btnGap * 5.0f;
            float bx = centerAxisX - totalBtnsW * 0.5f;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##ShuffleBtn", IconType::Shuffle, ImVec2(secBtnSz, secBtnSz), g_isShuffle, accentRGBA, hudAlpha)) {
                g_isShuffle = !g_isShuffle;
            }
            bx += secBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##PrevBtn", IconType::Prev, ImVec2(secBtnSz, secBtnSz), false, accentRGBA, hudAlpha)) {
                if (audio.getPlaybackPosition() > 3.0f) {
                    audio.seekTo(0.0f);
                } else {
                    const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                    int prevIdx = getPrevTrackIndex(listToUse);
                    if (prevIdx >= 0) {
                        g_selectedTrack = prevIdx;
                        g_activePlayingTrack = prevIdx;
                        discFlightProgress = 0.0f;
                        audio.playFile(g_playlist[prevIdx].widePath);
                        focusTrackOnShelf(prevIdx, g_visibleIndices);
                    }
                }
            }
            bx += secBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - mainBtnSz * 0.5f));
            IconType playIcon = audio.isPlaying() ? IconType::Pause : IconType::Play;
            if (IconButton("##PlayPauseBtn", playIcon, ImVec2(mainBtnSz, mainBtnSz), false, accentRGBA, hudAlpha, true)) {
                if (g_activePlayingTrack == -1 && !g_playlist.empty()) {
                    g_activePlayingTrack = displayTrackIdx;
                    g_selectedTrack = displayTrackIdx;
                    discFlightProgress = 0.0f;
                    audio.playFile(g_playlist[displayTrackIdx].widePath);
                } else {
                    audio.togglePlayPause();
                }
            }
            bx += mainBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##NextBtn", IconType::Next, ImVec2(secBtnSz, secBtnSz), false, accentRGBA, hudAlpha)) {
                const auto& listToUse = g_visibleIndices.empty() ? std::vector<int>{displayTrackIdx} : g_visibleIndices;
                int nextIdx = getNextTrackIndex(listToUse, true);
                if (nextIdx >= 0) {
                    g_selectedTrack = nextIdx;
                    g_activePlayingTrack = nextIdx;
                    discFlightProgress = 0.0f;
                    audio.playFile(g_playlist[nextIdx].widePath);
                    focusTrackOnShelf(nextIdx, g_visibleIndices);
                }
            }
            bx += secBtnSz + btnGap;

            LoopMode lm = audio.getLoopMode();
            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##LoopBtn", IconType::Repeat, ImVec2(secBtnSz, secBtnSz), lm != LoopMode::None, accentRGBA, hudAlpha)) {
                if (lm == LoopMode::All) audio.setLoopMode(LoopMode::Track);
                else if (lm == LoopMode::Track) audio.setLoopMode(LoopMode::None);
                else audio.setLoopMode(LoopMode::All);
            }
            if (lm == LoopMode::Track) {
                ImVec2 rMin = ImGui::GetItemRectMin();
                dl->AddText(ImVec2(rMin.x + 12.0f, rMin.y + 7.0f), ImColor(accentRGBA), "1");
            }
            bx += secBtnSz + btnGap;

            ImGui::SetCursorPos(ImVec2(bx, row1CenterY - secBtnSz * 0.5f));
            if (IconButton("##RouletteDockBtn", IconType::Dice, ImVec2(secBtnSz, secBtnSz), g_roulette.active, accentRGBA, hudAlpha)) {
                startRandomRoulette(g_visibleIndices, audio, discFlightProgress);
            }

            const float row2CenterY = 61.0f;
            const float barW = 260.0f;
            const float barH = 16.0f;
            const float barX = centerAxisX - barW * 0.5f;

            float curSec = audio.getPlaybackPosition();
            float totSec = audio.getDuration();
            std::string curTimeStr = formatTime(curSec);
            std::string totTimeStr = formatTime(totSec);

            ImVec2 curTimeSz = ImGui::CalcTextSize(curTimeStr.c_str());
            ImVec2 totTimeSz = ImGui::CalcTextSize(totTimeStr.c_str());

            ImGui::SetCursorPos(ImVec2(barX - curTimeSz.x - 10.0f, row2CenterY - curTimeSz.y * 0.5f));
            ImGui::TextColored(accentRGBA, "%s", curTimeStr.c_str());

            ImGui::SetCursorPos(ImVec2(barX, row2CenterY - barH * 0.5f));
            float seekVal = curSec;
            if (CustomScrubber("##CenteredScrubber", &seekVal, totSec, barW, barH, accentRGBA)) {
                audio.seekTo((totSec > 0.0f) ? (seekVal / totSec) : 0.0f);
            }

            ImGui::SetCursorPos(ImVec2(barX + barW + 10.0f, row2CenterY - totTimeSz.y * 0.5f));
            ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.88f, hudAlpha), "%s", totTimeStr.c_str());

            const float rightCenterY = dockH * 0.5f;
            const float volBarW = 86.0f;
            const float volBarH = 16.0f;
            const float muteBtnSz = 28.0f;
            const float rightPad = 22.0f;

            float volBarX = dockW - rightPad - volBarW;
            float muteBtnX = volBarX - 8.0f - muteBtnSz;

            ImGui::SetCursorPos(ImVec2(muteBtnX, rightCenterY - muteBtnSz * 0.5f));
            IconType volIcon = (audio.isMuted() || audio.getVolume() <= 0.001f) ? IconType::Mute : IconType::Volume;
            if (IconButton("##MuteBtn", volIcon, ImVec2(muteBtnSz, muteBtnSz), audio.isMuted(), accentRGBA, hudAlpha)) {
                audio.toggleMute();
            }

            ImGui::SetCursorPos(ImVec2(volBarX, rightCenterY - volBarH * 0.5f));
            float vol = audio.isMuted() ? 0.0f : audio.getVolume();
            if (CustomScrubber("##VolScrubber", &vol, 1.0f, volBarW, volBarH, accentRGBA)) {
                audio.setVolume(vol);
            }

            ImGui::End();
            ImGui::PopStyleColor(3);
        }

        // ------------------ 4. КЛИКИ ПО БОКСАМ И КОНТЕКСТНОЕ МЕНЮ ПО ПКМ ------------------
        if (!io.WantCaptureMouse && !g_roulette.active && mouseY >= 72.0) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverIndex != -1 && !g_playlist.empty()) {
                int targetIdx = std::clamp(hoverIndex, 0, static_cast<int>(g_playlist.size()) - 1);
                g_selectedTrack = targetIdx;
                discFlightProgress = 0.0f;
                if (g_activePlayingTrack != targetIdx) {
                    g_activePlayingTrack = targetIdx;
                    audio.playFile(g_playlist[targetIdx].widePath);
                } else {
                    audio.togglePlayPause();
                }
                focusTrackOnShelf(targetIdx, g_visibleIndices);
                lastActiveTime = now;
            } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                       glm::length(glm::vec2(io.MouseDelta.x, io.MouseDelta.y)) < 2.0f &&
                       hoverIndex != -1) {
                g_contextMenuTrack = hoverIndex;
                ImGui::OpenPopup("TrackContextMenu");
                lastActiveTime = now;
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.10f, 0.14f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.99f, 1.0f));
        if (ImGui::BeginPopup("TrackContextMenu")) {
            lastActiveTime = now;
            if (g_contextMenuTrack >= 0 && g_contextMenuTrack < static_cast<int>(g_playlist.size())) {
                const auto& ct = g_playlist[g_contextMenuTrack];
                ImGui::TextColored(ImVec4(ct.accentColor.r, ct.accentColor.g, ct.accentColor.b, 1.0f), "%s", truncateUtf8(ct.title, 26).c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Воспроизвести")) {
                    g_selectedTrack = g_contextMenuTrack;
                    g_activePlayingTrack = g_contextMenuTrack;
                    discFlightProgress = 0.0f;
                    audio.playFile(ct.widePath);
                    focusTrackOnShelf(g_contextMenuTrack, g_visibleIndices);
                }
                if (ImGui::MenuItem("Показать в проводнике")) {
                    std::wstring param = L"/select,\"" + ct.widePath + L"\"";
                    ShellExecuteW(hwnd, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                }
                ImGui::Separator();
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
                if (ImGui::MenuItem("Удалить из коллекции")) {
                    if (g_activePlayingTrack == g_contextMenuTrack) {
                        audio.stop();
                        g_activePlayingTrack = -1;
                        g_selectedTrack = -1;
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
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        double targetFrameTime = (now - lastActiveTime < 0.40) ? (1.0 / 100.0) : (1.0 / 30.0);
        double elapsed = glfwGetTime() - frameStart;
        if (elapsed < targetFrameTime) {
            DWORD sleepMs = static_cast<DWORD>((targetFrameTime - elapsed) * 1000.0);
            if (sleepMs > 0) {
                Sleep(sleepMs);
            }
        }
    }

    saveSettingsToDisk(audio);

    for (auto& track : g_playlist) {
        MetadataParser::cleanup(track);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwTerminate();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
