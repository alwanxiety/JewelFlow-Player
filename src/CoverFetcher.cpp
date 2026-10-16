#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "CoverFetcher.hpp"
#include "Metadata.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>

namespace fs = std::filesystem;

static std::string trimCopy(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n\0");
    if (first == std::string::npos) return "";
    auto last = s.find_last_not_of(" \t\r\n\0");
    return s.substr(first, last - first + 1);
}

static std::string cleanTrackQuery(const std::string& text) {
    std::string q = text;
    try {
        static const std::regex reUrls(R"(https?://\S+|www\.\S+)", std::regex::icase);
        static const std::regex reBrackets(R"(\[[^\]]*\])");
        static const std::regex reFeatParen(R"(\((feat|ft|prod|with)\.?[^\)]*\))", std::regex::icase);
        static const std::regex reFeatWord(R"(\b(feat|ft|prod)\b.*)", std::regex::icase);
        static const std::regex reRemaster(R"(\b(remaster(ed)?|anniversary|deluxe|bonus|version|edition)\b.*)", std::regex::icase);

        q = std::regex_replace(q, reUrls, "");
        q = std::regex_replace(q, reBrackets, "");
        q = std::regex_replace(q, reFeatParen, "");
        q = std::regex_replace(q, reFeatWord, "");
        q = std::regex_replace(q, reRemaster, "");
    } catch (...) {}
    return trimCopy(q);
}

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

static std::string httpGetUtf8(const std::wstring& host, const std::wstring& pathAndQuery, const std::wstring& userAgent) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(userAgent.c_str(),
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, 4000, 4000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathAndQuery.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    BOOL bResults = WinHttpSendRequest(hRequest,
                                       L"Accept: application/json, text/plain, */*\r\n",
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
                std::string chunk(dwSize, 0);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, chunk.data(), dwSize, &dwDownloaded)) {
                    chunk.resize(dwDownloaded);
                    result.append(chunk);
                } else {
                    break;
                }
            } while (dwSize > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

static std::vector<uint8_t> httpGetBinary(const std::wstring& host, const std::wstring& pathAndQuery, const std::wstring& userAgent) {
    std::vector<uint8_t> result;
    HINTERNET hSession = WinHttpOpen(userAgent.c_str(),
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, 4500, 4500, 6000, 6000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathAndQuery.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    BOOL bResults = WinHttpSendRequest(hRequest,
                                       L"Accept: image/jpeg, image/png, image/*, */*\r\n",
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
                size_t curSize = result.size();
                result.resize(curSize + dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, result.data() + curSize, dwSize, &dwDownloaded)) {
                    result.resize(curSize + dwDownloaded);
                } else {
                    result.resize(curSize);
                    break;
                }
            } while (dwSize > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

static bool parseUrlComponents(const std::string& url, std::wstring& outHost, std::wstring& outPath) {
    std::string s = url;
    if (s.rfind("https://", 0) == 0) {
        s = s.substr(8);
    } else if (s.rfind("http://", 0) == 0) {
        s = s.substr(7);
    }

    auto slashPos = s.find('/');
    if (slashPos == std::string::npos) {
        outHost = MetadataParser::utf8ToWide(s);
        outPath = L"/";
    } else {
        outHost = MetadataParser::utf8ToWide(s.substr(0, slashPos));
        outPath = MetadataParser::utf8ToWide(s.substr(slashPos));
    }
    return !outHost.empty();
}

static std::string extractJsonString(const std::string& json, const std::string& fieldName) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";

    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return "";

    size_t valStart = colon + 1;
    while (valStart < json.size() && (json[valStart] == ' ' || json[valStart] == '\t' || json[valStart] == '\r' || json[valStart] == '\n')) {
        valStart++;
    }

    if (valStart >= json.size() || json[valStart] != '"') return "";

    std::string decoded;
    size_t i = valStart + 1;
    while (i < json.size()) {
        char c = json[i++];
        if (c == '"') break;
        if (c == '\\' && i < json.size()) {
            char esc = json[i++];
            if (esc == '/') decoded.push_back('/');
            else if (esc == '\\') decoded.push_back('\\');
            else if (esc == '"') decoded.push_back('"');
            else decoded.push_back(esc);
        } else {
            decoded.push_back(c);
        }
    }
    return decoded;
}

CoverFetcher::CoverFetcher() {
    m_running = true;
    m_workerThread = std::thread(&CoverFetcher::workerLoop, this);
}

CoverFetcher::~CoverFetcher() {
    m_running = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

fs::path CoverFetcher::getCoverCacheDir() {
    wchar_t exePath[MAX_PATH] = {0};
    fs::path baseDir;
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        baseDir = fs::path(exePath).parent_path();
    } else {
        baseDir = fs::current_path();
    }
    fs::path dir = baseDir / "covers_cache";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string CoverFetcher::computeTrackCoverHash(const std::string& artist, const std::string& title) {
    std::string s = artist + "__" + title;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    uint64_t hash = 14695981039346656037ULL;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

fs::path CoverFetcher::getCachedCoverPath(const std::string& artist, const std::string& title) {
    std::string h = computeTrackCoverHash(artist, title);
    return getCoverCacheDir() / (h + ".jpg");
}

void CoverFetcher::requestCover(const std::wstring& widePath,
                                const std::string& artist,
                                const std::string& title,
                                const std::string& album,
                                bool forceRefresh) {
    if (widePath.empty() || title.empty()) return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!forceRefresh && m_inFlight.count(widePath)) return;
    m_inFlight.insert(widePath);
    m_queue.push_back({widePath, artist, title, album, forceRefresh});
    m_cv.notify_one();
}

std::vector<CoverResult> CoverFetcher::pollCompleted() {
    std::unique_lock<std::mutex> lock(m_mutex);
    std::vector<CoverResult> res;
    if (!m_completed.empty()) {
        res = std::move(m_completed);
        m_completed.clear();
    }
    return res;
}

void CoverFetcher::workerLoop() {
    while (m_running) {
        RequestItem req;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return !m_running || !m_queue.empty();
            });
            if (!m_running) break;
            req = std::move(m_queue.front());
            m_queue.pop_front();
        }

        std::vector<uint8_t> imgBytes;
        bool ok = fetchCoverOnline(req, imgBytes);

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_inFlight.erase(req.widePath);
            if (ok && !imgBytes.empty()) {
                m_completed.push_back({req.widePath, std::move(imgBytes)});
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool CoverFetcher::fetchCoverOnline(const RequestItem& req, std::vector<uint8_t>& outImage) {
    fs::path cachePath = getCachedCoverPath(req.artist, req.title);
    std::error_code ec;

    if (!req.forceRefresh && fs::exists(cachePath, ec)) {
        std::ifstream f(cachePath, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            auto sz = f.tellg();
            if (sz > 500 && sz < 20 * 1024 * 1024) {
                f.seekg(0, std::ios::beg);
                outImage.resize(static_cast<size_t>(sz));
                if (f.read(reinterpret_cast<char*>(outImage.data()), sz)) {
                    return true;
                }
            }
        }
    }

    std::string cleanArtist = cleanTrackQuery(req.artist);
    std::string cleanTitle = cleanTrackQuery(req.title);

    if (cleanArtist == "Unknown Artist" || cleanArtist.empty()) {
        cleanArtist.clear();
    }

    std::string query = cleanArtist.empty() ? cleanTitle : (cleanArtist + " " + cleanTitle);
    if (query.empty()) return false;

    std::string artworkUrl;

    // 1. Поиск через iTunes Search API (высокое качество до 1000x1000 / 600x600)
    if (searchItunes(query, artworkUrl)) {
        if (downloadBinaryFromUrl(artworkUrl, outImage)) {
            // Сохраняем в кэш
            std::ofstream out(cachePath, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(outImage.data()), outImage.size());
            }
            return true;
        }
    }

    // 2. Запасной поиск через Deezer API
    if (searchDeezer(query, artworkUrl)) {
        if (downloadBinaryFromUrl(artworkUrl, outImage)) {
            std::ofstream out(cachePath, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(outImage.data()), outImage.size());
            }
            return true;
        }
    }

    return false;
}

bool CoverFetcher::searchItunes(const std::string& query, std::string& outArtworkUrl) {
    std::wstring host = L"itunes.apple.com";
    std::wstring path = L"/search?term=" + MetadataParser::utf8ToWide(urlEncodeUtf8(query)) + L"&entity=song&limit=1";
    std::wstring ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

    std::string json = httpGetUtf8(host, path, ua);
    if (json.empty()) return false;

    std::string art = extractJsonString(json, "artworkUrl100");
    if (art.empty()) {
        art = extractJsonString(json, "artworkUrl60");
    }
    if (art.empty()) return false;

    // Преобразуем превью 100x100 в полноразмерную студийную обложку 1000x1000
    auto pos = art.find("100x100bb");
    if (pos != std::string::npos) {
        art.replace(pos, 9, "1000x1000bb");
    } else {
        pos = art.find("60x60bb");
        if (pos != std::string::npos) {
            art.replace(pos, 7, "1000x1000bb");
        }
    }

    outArtworkUrl = art;
    return true;
}

bool CoverFetcher::searchDeezer(const std::string& query, std::string& outArtworkUrl) {
    std::wstring host = L"api.deezer.com";
    std::wstring path = L"/search?q=" + MetadataParser::utf8ToWide(urlEncodeUtf8(query)) + L"&limit=1";
    std::wstring ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

    std::string json = httpGetUtf8(host, path, ua);
    if (json.empty()) return false;

    std::string art = extractJsonString(json, "cover_xl");
    if (art.empty()) art = extractJsonString(json, "cover_big");
    if (art.empty()) art = extractJsonString(json, "cover_medium");
    if (art.empty()) return false;

    outArtworkUrl = art;
    return true;
}

bool CoverFetcher::downloadBinaryFromUrl(const std::string& url, std::vector<uint8_t>& outData) {
    std::wstring host, path;
    if (!parseUrlComponents(url, host, path)) return false;

    std::wstring ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    outData = httpGetBinary(host, path, ua);
    return (outData.size() > 500);
}
