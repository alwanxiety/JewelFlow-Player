#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include "YandexMusicManager.hpp"
#include "Metadata.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <set>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

static std::string urlEncodeUtf8(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << "%20";
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

static std::string httpGetLocalUtf8(const std::wstring& pathAndQuery, int timeoutMs = 8000) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"JewelPlayer/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, 2000, 2000, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 19840, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathAndQuery.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    BOOL bResults = WinHttpSendRequest(hRequest,
                                       L"Accept: application/json\r\n",
                                       (DWORD)-1L,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, nullptr);
    }

    if (bResults) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode >= 200 && statusCode < 300) {
            DWORD dwSize = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                std::vector<char> buf(dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                    result.append(buf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

static void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static std::string extractJsonString(const std::string& json, const std::string& fieldName, size_t startPos = 0) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return "";

    size_t valStart = json.find('"', colon + 1);
    if (valStart == std::string::npos) return "";
    valStart++;

    std::string decoded;
    decoded.reserve(32);
    for (size_t i = valStart; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"' && (i == valStart || json[i - 1] != '\\')) {
            return decoded;
        }
        if (c == '\\' && i + 1 < json.size()) {
            char esc = json[++i];
            switch (esc) {
                case '"':  decoded.push_back('"'); break;
                case '\\': decoded.push_back('\\'); break;
                case '/':  decoded.push_back('/'); break;
                case 'b':  decoded.push_back('\b'); break;
                case 'f':  decoded.push_back('\f'); break;
                case 'n':  decoded.push_back('\n'); break;
                case 'r':  decoded.push_back('\r'); break;
                case 't':  decoded.push_back('\t'); break;
                case 'u': {
                    if (i + 4 < json.size()) {
                        std::string hexStr = json.substr(i + 1, 4);
                        i += 4;
                        try {
                            uint32_t cp = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                                std::string lowHex = json.substr(i + 3, 4);
                                try {
                                    uint32_t low = static_cast<uint32_t>(std::stoul(lowHex, nullptr, 16));
                                    if (low >= 0xDC00 && low <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                        i += 6;
                                    }
                                } catch (...) {}
                            }
                            if (cp != 0) appendUtf8Codepoint(decoded, cp);
                        } catch (...) {}
                    }
                    break;
                }
                default: decoded.push_back(esc); break;
            }
        } else {
            decoded.push_back(c);
        }
    }
    return decoded;
}

static int extractJsonInt(const std::string& json, const std::string& fieldName, size_t startPos = 0) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return 0;
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return 0;
    size_t numStart = colon + 1;
    while (numStart < json.size() && (json[numStart] == ' ' || json[numStart] == '\t' || json[numStart] == '\r' || json[numStart] == '\n')) {
        numStart++;
    }
    if (numStart >= json.size()) return 0;
    try {
        return std::stoi(json.substr(numStart));
    } catch (...) {
        return 0;
    }
}

static bool extractJsonBool(const std::string& json, const std::string& fieldName, size_t startPos = 0) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return false;
    size_t valStart = colon + 1;
    while (valStart < json.size() && (json[valStart] == ' ' || json[valStart] == '\t' || json[valStart] == '\r' || json[valStart] == '\n')) {
        valStart++;
    }
    if (valStart + 4 <= json.size() && json.compare(valStart, 4, "true") == 0) return true;
    return false;
}

static std::vector<std::string> extractJsonObjects(const std::string& json) {
    std::vector<std::string> objects;
    size_t arrayStart = json.find('[');
    if (arrayStart == std::string::npos) return objects;

    int depth = 0;
    bool inString = false;
    size_t objStart = std::string::npos;

    for (size_t i = arrayStart; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"' && (i == 0 || json[i - 1] != '\\')) {
            inString = !inString;
        } else if (!inString) {
            if (c == '{') {
                if (depth == 0) objStart = i;
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0 && objStart != std::string::npos) {
                    objects.push_back(json.substr(objStart, i - objStart + 1));
                    objStart = std::string::npos;
                    if (objects.size() >= 50) break;
                }
            } else if (c == ']' && depth == 0) {
                break;
            }
        }
    }
    return objects;
}

} // anonymous namespace

struct YandexMusicManager::Impl {
    fs::path exeDir;
    std::atomic<bool> isBridgeRunning{false};
    std::atomic<bool> isAuthenticated{false};
    std::string userName;
    std::string authMessage;
    std::atomic<bool> isAuthBusy{false};
    mutable std::mutex authMutex;

    std::atomic<uint64_t> searchSeq{0};
    std::atomic<bool> isSearching{false};
    std::vector<YmTrack> searchResults;
    mutable std::mutex searchMutex;

    std::set<std::string> downloadingTrackIds;
    std::vector<YmCompletedDownload> completedDownloads;
    mutable std::mutex downloadMutex;

    std::atomic<bool> shutdownRequested{false};
    std::thread statusThread;

    void launchBridgeProcess() {
        std::wstring dirArg = exeDir.wstring();

        // 1. Приоритет №1 (для релизных сборок на GitHub):
        // Скомпилированный автономный ym_bridge.exe (пользователю не нужен Python вообще!)
        fs::path bridgeExe = exeDir / "ym_bridge.exe";
        if (!fs::exists(bridgeExe)) {
            bridgeExe = fs::current_path() / "ym_bridge.exe";
        }
        if (!fs::exists(bridgeExe)) {
            bridgeExe = fs::current_path() / "dist" / "ym_bridge.exe";
        }

        if (fs::exists(bridgeExe)) {
            std::wstring cmdLine = L"\"" + bridgeExe.wstring() + L"\"";
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi{};
            DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, flags, nullptr, dirArg.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return;
            }
        }

        // 2. Приоритет №2 (для разработчиков, запускающих из исходников):
        // Исходный скрипт ym_bridge.py через локальный интерпретатор Python
        fs::path scriptPath = exeDir / "ym_bridge.py";
        if (!fs::exists(scriptPath)) {
            scriptPath = fs::current_path() / "ym_bridge.py";
        }
        if (!fs::exists(scriptPath)) {
            scriptPath = fs::path("ym_bridge.py");
        }

        std::wstring scriptArg = L"\"" + scriptPath.wstring() + L"\"";

        // Собираем пути к интерпретаторам Python
        std::vector<std::wstring> candidates;

        // Поиск в стандартных каталогах установки пользователя (AppData\Local\Programs\Python)
        wchar_t* localAppData = nullptr;
        if (_wdupenv_s(&localAppData, nullptr, L"LOCALAPPDATA") == 0 && localAppData) {
            fs::path pyRoot = fs::path(localAppData) / L"Programs" / L"Python";
            std::error_code ec;
            if (fs::exists(pyRoot, ec)) {
                for (const auto& entry : fs::directory_iterator(pyRoot, ec)) {
                    if (entry.is_directory(ec)) {
                        fs::path pw = entry.path() / L"pythonw.exe";
                        if (fs::exists(pw, ec)) candidates.push_back(pw.wstring());
                        fs::path p = entry.path() / L"python.exe";
                        if (fs::exists(p, ec)) candidates.push_back(p.wstring());
                    }
                }
            }
            free(localAppData);
        }

        // Общие имена в PATH
        candidates.push_back(L"pythonw.exe");
        candidates.push_back(L"pyw.exe");
        candidates.push_back(L"python.exe");
        candidates.push_back(L"py.exe");

        // Запуск через CreateProcessW с флагами CREATE_NO_WINDOW | DETACHED_PROCESS
        for (const auto& launcher : candidates) {
            std::wstring cmdLine = L"\"" + launcher + L"\" " + scriptArg;
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi{};
            DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, flags, nullptr, dirArg.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                break;
            }
        }
    }

    void queryStatusDirect() {
        std::string json = httpGetLocalUtf8(L"/status", 1500);
        if (!json.empty()) {
            isBridgeRunning = true;
            bool auth = extractJsonBool(json, "authenticated");
            std::string user = extractJsonString(json, "user");
            {
                std::lock_guard<std::mutex> lock(authMutex);
                isAuthenticated = auth;
                userName = user;
                if (auth) {
                    authMessage = "Авторизован как: " + (user.empty() ? std::string("Пользователь") : user);
                } else {
                    authMessage = "";
                }
            }
        }
    }
};

YandexMusicManager& YandexMusicManager::instance() {
    static YandexMusicManager s_instance;
    return s_instance;
}

YandexMusicManager::YandexMusicManager() : m_impl(std::make_unique<Impl>()) {}

YandexMusicManager::~YandexMusicManager() {
    shutdown();
}

void YandexMusicManager::init(const fs::path& exeDir) {
    if (!m_impl) return;
    m_impl->exeDir = exeDir;

    // 1. Запуск процесса локального HTTP-моста
    m_impl->launchBridgeProcess();

    // 2. Фоновый поток периодической проверки доступности моста и статуса
    m_impl->statusThread = std::thread([this]() {
        for (int i = 0; i < 30 && !m_impl->shutdownRequested; ++i) {
            m_impl->queryStatusDirect();
            if (m_impl->isBridgeRunning) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        // Периодическая проверка раз в 10 секунд
        while (!m_impl->shutdownRequested) {
            for (int k = 0; k < 100 && !m_impl->shutdownRequested; ++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!m_impl->shutdownRequested) {
                m_impl->queryStatusDirect();
            }
        }
    });
}

void YandexMusicManager::shutdown() {
    if (!m_impl) return;
    m_impl->shutdownRequested = true;
    if (m_impl->statusThread.joinable()) {
        m_impl->statusThread.join();
    }
}

bool YandexMusicManager::isAuthenticated() const {
    return m_impl ? m_impl->isAuthenticated.load() : false;
}

std::string YandexMusicManager::getUserName() const {
    if (!m_impl) return "";
    std::lock_guard<std::mutex> lock(m_impl->authMutex);
    return m_impl->userName;
}

bool YandexMusicManager::isBridgeRunning() const {
    return m_impl ? m_impl->isBridgeRunning.load() : false;
}

void YandexMusicManager::refreshAuthStatus() {
    if (!m_impl) return;
    std::thread([this]() {
        m_impl->queryStatusDirect();
    }).detach();
}

void YandexMusicManager::submitTokenAsync(const std::string& tokenOrUrl, std::function<void(bool success, const std::string& msg)> onDone) {
    if (!m_impl) return;
    m_impl->isAuthBusy = true;
    {
        std::lock_guard<std::mutex> lock(m_impl->authMutex);
        m_impl->authMessage = "Проверка токена в Яндекс Музыке...";
    }

    std::thread([this, tokenOrUrl, onDone]() {
        std::string encoded = urlEncodeUtf8(tokenOrUrl);
        std::wstring path = L"/auth?token=" + MetadataParser::utf8ToWide(encoded);
        std::string resp = httpGetLocalUtf8(path, 15000);

        bool success = false;
        std::string msg;

        if (!resp.empty()) {
            std::string st = extractJsonString(resp, "status");
            if (st == "ok") {
                success = true;
                bool auth = extractJsonBool(resp, "authenticated");
                std::string user = extractJsonString(resp, "user");
                {
                    std::lock_guard<std::mutex> lock(m_impl->authMutex);
                    m_impl->isAuthenticated = auth;
                    m_impl->userName = user;
                    m_impl->authMessage = "Успешно авторизован как: " + user;
                    msg = m_impl->authMessage;
                }
            } else {
                std::string err = extractJsonString(resp, "message");
                if (err.empty()) err = "Не удалось авторизоваться";
                {
                    std::lock_guard<std::mutex> lock(m_impl->authMutex);
                    m_impl->authMessage = err;
                    msg = err;
                }
            }
        } else {
            msg = "Нет ответа от локального моста Яндекс Музыки (127.0.0.1:19840)";
            std::lock_guard<std::mutex> lock(m_impl->authMutex);
            m_impl->authMessage = msg;
        }

        m_impl->isAuthBusy = false;
        if (onDone) {
            onDone(success, msg);
        }
    }).detach();
}

void YandexMusicManager::logoutAsync(std::function<void()> onDone) {
    if (!m_impl) return;
    std::thread([this, onDone]() {
        httpGetLocalUtf8(L"/logout", 5000);
        {
            std::lock_guard<std::mutex> lock(m_impl->authMutex);
            m_impl->isAuthenticated = false;
            m_impl->userName = "";
            m_impl->authMessage = "Вы вышли из аккаунта";
        }
        if (onDone) onDone();
    }).detach();
}

void YandexMusicManager::searchAsync(const std::string& query) {
    if (!m_impl) return;

    if (query.empty()) {
        clearSearchResults();
        return;
    }

    uint64_t seq = ++m_impl->searchSeq;
    m_impl->isSearching = true;

    std::thread([this, query, seq]() {
        std::string encoded = urlEncodeUtf8(query);
        std::wstring path = L"/search?q=" + MetadataParser::utf8ToWide(encoded);
        std::string resp = httpGetLocalUtf8(path, 10000);

        if (seq != m_impl->searchSeq) {
            // Устаревший запрос — игнорируем
            return;
        }

        std::vector<YmTrack> parsedTracks;
        if (!resp.empty() && resp[0] == '[') {
            auto objs = extractJsonObjects(resp);
            parsedTracks.reserve(objs.size());
            for (const auto& obj : objs) {
                YmTrack t;
                t.id = extractJsonString(obj, "id");
                t.title = extractJsonString(obj, "title");
                t.artist = extractJsonString(obj, "artist");
                t.album = extractJsonString(obj, "album");
                t.duration = extractJsonInt(obj, "duration");
                t.coverUrl = extractJsonString(obj, "cover_url");
                t.thumbPath = extractJsonString(obj, "thumb_path");
                if (!t.id.empty() && !t.title.empty()) {
                    parsedTracks.push_back(std::move(t));
                }
            }
        }

        if (seq == m_impl->searchSeq) {
            std::lock_guard<std::mutex> lock(m_impl->searchMutex);
            m_impl->searchResults = std::move(parsedTracks);
            m_impl->isSearching = false;
        }
    }).detach();
}

bool YandexMusicManager::isSearching() const {
    return m_impl ? m_impl->isSearching.load() : false;
}

std::vector<YmTrack> YandexMusicManager::getSearchResults() {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->searchMutex);
    return m_impl->searchResults;
}

void YandexMusicManager::clearSearchResults() {
    if (!m_impl) return;
    m_impl->searchSeq++;
    m_impl->isSearching = false;
    std::lock_guard<std::mutex> lock(m_impl->searchMutex);
    m_impl->searchResults.clear();
}

void YandexMusicManager::downloadTrackAsync(const YmTrack& track) {
    if (!m_impl || track.id.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_impl->downloadMutex);
        if (m_impl->downloadingTrackIds.count(track.id)) {
            // Уже скачивается
            return;
        }
        m_impl->downloadingTrackIds.insert(track.id);
    }

    std::thread([this, track]() {
        std::wstring path = L"/download?id=" + MetadataParser::utf8ToWide(urlEncodeUtf8(track.id));
        std::string resp = httpGetLocalUtf8(path, 35000);

        if (!resp.empty()) {
            std::string st = extractJsonString(resp, "status");
            if (st == "ok") {
                std::string filePath = extractJsonString(resp, "path");
                std::string coverFilePath = extractJsonString(resp, "cover_path");
                std::string respTitle = extractJsonString(resp, "title");
                std::string respArtist = extractJsonString(resp, "artist");
                std::string respAlbum = extractJsonString(resp, "album");
                int respDur = extractJsonInt(resp, "duration");
                if (respDur <= 0) respDur = track.duration;
                if (!filePath.empty()) {
                    std::wstring widePath = MetadataParser::utf8ToWide(filePath);
                    std::wstring wideCover = MetadataParser::utf8ToWide(coverFilePath);
                    if (fs::exists(widePath)) {
                        std::lock_guard<std::mutex> lock(m_impl->downloadMutex);
                        m_impl->completedDownloads.push_back({
                            track.id,
                            widePath,
                            wideCover,
                            respTitle.empty() ? track.title : respTitle,
                            respArtist.empty() ? track.artist : respArtist,
                            respAlbum.empty() ? track.album : respAlbum,
                            respDur
                        });
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_impl->downloadMutex);
            m_impl->downloadingTrackIds.erase(track.id);
        }
    }).detach();
}

bool YandexMusicManager::isTrackDownloading(const std::string& trackId) const {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lock(m_impl->downloadMutex);
    return m_impl->downloadingTrackIds.count(trackId) > 0;
}

std::vector<YmCompletedDownload> YandexMusicManager::pollCompletedDownloads() {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->downloadMutex);
    std::vector<YmCompletedDownload> result = std::move(m_impl->completedDownloads);
    m_impl->completedDownloads.clear();
    return result;
}

std::string YandexMusicManager::getAuthMessage() const {
    if (!m_impl) return "";
    std::lock_guard<std::mutex> lock(m_impl->authMutex);
    return m_impl->authMessage;
}

bool YandexMusicManager::isAuthBusy() const {
    return m_impl ? m_impl->isAuthBusy.load() : false;
}
