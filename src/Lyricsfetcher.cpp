#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "LyricsFetcher.hpp"
#include "Metadata.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <iomanip>

namespace fs = std::filesystem;

static std::string trimCopy(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n\0");
    if (first == std::string::npos) return "";
    auto last = s.find_last_not_of(" \t\r\n\0");
    return s.substr(first, last - first + 1);
}

static void replaceAllInPlace(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
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

// Выполняет HTTPS GET запрос через нативный WinHTTP (с поддержкой редиректов, TLS и автоповтора при 503/429)
static std::string httpGetUtf8(const std::wstring& host, const std::wstring& pathAndQuery, const std::wstring& userAgent = L"") {
    const std::wstring defaultUA = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 JewelBoxPlayer/2.0";
    const std::wstring& ua = userAgent.empty() ? defaultUA : userAgent;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            Sleep(350 * attempt);
        }

        std::string result;
        HINTERNET hSession = WinHttpOpen(ua.c_str(),
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) continue;

        WinHttpSetTimeouts(hSession, 4500, 4500, 6000, 6000);

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            continue;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathAndQuery.c_str(),
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            continue;
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        BOOL bResults = WinHttpSendRequest(hRequest,
                                           L"Accept: application/json, text/plain, */*\r\nAccept-Language: ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7\r\n",
                                           (DWORD)-1L,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, nullptr);
        }

        DWORD statusCode = 0;
        if (bResults) {
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        }

        if (statusCode >= 200 && statusCode < 300) {
            DWORD dwSize = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                std::vector<char> buffer(dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                    result.append(buffer.data(), dwDownloaded);
                }
                if (result.size() > 4 * 1024 * 1024) break;
            } while (dwSize > 0);

            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        // Если 503 (ServerOverloaded у LRCLIB) или 429 (Too Many Requests), делаем паузу и повторяем запрос
        if (statusCode == 503 || statusCode == 429) {
            continue;
        }
        if (statusCode >= 400 && statusCode < 500) {
            return "";
        }
    }
    return "";
}

// Быстрый парсер JSON-строки по ключу (корректно обрабатывает экранирование \n, \", \\ и \uXXXX в UTF-8)
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

static std::string extractJsonStringField(const std::string& json, const std::string& fieldName, size_t startSearchPos = 0, size_t* outNextPos = nullptr) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = startSearchPos;

    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t colon = json.find(':', pos + pattern.size());
        if (colon == std::string::npos) break;

        size_t valStart = colon + 1;
        while (valStart < json.size() && (json[valStart] == ' ' || json[valStart] == '\t' || json[valStart] == '\r' || json[valStart] == '\n')) {
            valStart++;
        }

        if (valStart < json.size() && json.compare(valStart, 4, "null") == 0) {
            pos = valStart + 4;
            continue;
        }

        if (valStart >= json.size() || json[valStart] != '"') {
            pos = valStart + 1;
            continue;
        }

        std::string decoded;
        size_t i = valStart + 1;
        while (i < json.size()) {
            char c = json[i++];
            if (c == '"') {
                if (outNextPos) *outNextPos = i;
                if (!decoded.empty()) return decoded;
                break;
            }
            if (c == '\\' && i < json.size()) {
                char esc = json[i++];
                switch (esc) {
                    case '"':  decoded.push_back('"'); break;
                    case '\\': decoded.push_back('\\'); break;
                    case '/':  decoded.push_back('/'); break;
                    case 'b':  decoded.push_back('\b'); break;
                    case 'f':  decoded.push_back('\f'); break;
                    case 'n':  decoded.push_back('\n'); break;
                    case 'r':  break;
                    case 't':  decoded.push_back('\t'); break;
                    case 'u': {
                        if (i + 4 <= json.size()) {
                            uint32_t cp = 0;
                            try {
                                cp = static_cast<uint32_t>(std::stoul(json.substr(i, 4), nullptr, 16));
                            } catch (...) { cp = 0; }
                            i += 4;
                            // Обработка суррогатной пары UTF-16 (\uD83C\uDFB5 и т.п.)
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= json.size() && json[i] == '\\' && json[i + 1] == 'u') {
                                try {
                                    uint32_t low = static_cast<uint32_t>(std::stoul(json.substr(i + 2, 4), nullptr, 16));
                                    if (low >= 0xDC00 && low <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                        i += 6;
                                    }
                                } catch (...) {}
                            }
                            if (cp != 0) appendUtf8Codepoint(decoded, cp);
                        }
                        break;
                    }
                    default: decoded.push_back(esc); break;
                }
            } else {
                decoded.push_back(c);
            }
        }
        pos = i;
    }
    return "";
}

static float extractJsonNumberField(const std::string& json, const std::string& fieldName, size_t startPos = 0) {
    std::string pattern = "\"" + fieldName + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return 0.0f;
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return 0.0f;
    size_t numStart = colon + 1;
    while (numStart < json.size() && (json[numStart] == ' ' || json[numStart] == '\t' || json[numStart] == '\r' || json[numStart] == '\n')) {
        numStart++;
    }
    if (numStart >= json.size()) return 0.0f;
    try {
        return std::stof(json.substr(numStart));
    } catch (...) {
        return 0.0f;
    }
}

static std::vector<std::string> extractJsonObjectsFromArray(const std::string& json) {
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
                    if (objects.size() >= 25) break;
                }
            } else if (c == ']' && depth == 0) {
                break;
            }
        }
    }
    return objects;
}

// Полный порт очистки запроса из Workers.cpp
std::string LyricsFetcher::cleanQuery(const std::string& text) {
    std::string q = text;
    try {
        static const std::regex reUrls(R"(https?://\S+|www\.\S+)", std::regex::icase);
        static const std::regex reBrackets(R"(\[[^\]]*\])");
        static const std::regex reFeatParen(R"(\((feat|ft|prod|with)\.?[^\)]*\))", std::regex::icase);
        static const std::regex reFeatWord(R"(\b(feat|ft|prod)\b.*)", std::regex::icase);

        q = std::regex_replace(q, reUrls, "");
        q = std::regex_replace(q, reBrackets, "");
        q = std::regex_replace(q, reFeatParen, "");
        q = std::regex_replace(q, reFeatWord, "");
    } catch (...) {}
    return trimCopy(q);
}

// Извлечение основного исполнителя (при наличии нескольких через запятую, слэш, feat и т.д.)
static std::string extractPrimaryArtist(const std::string& artist) {
    std::string a = trimCopy(artist);
    size_t comma = a.find(',');
    if (comma != std::string::npos) a = a.substr(0, comma);
    size_t slash = a.find('/');
    if (slash != std::string::npos) a = a.substr(0, slash);
    size_t feat = a.find(" feat");
    if (feat != std::string::npos) a = a.substr(0, feat);
    size_t ft = a.find(" ft.");
    if (ft != std::string::npos) a = a.substr(0, ft);
    size_t amp = a.find(" & ");
    if (amp != std::string::npos) a = a.substr(0, amp);
    return trimCopy(a);
}

// Парсер HTML страницы песни на Genius (точное извлечение всех блоков data-lyrics-container="true")
static std::string extractGeniusLyricsFromHtml(const std::string& html) {
    std::string combined;
    size_t searchPos = 0;

    while (true) {
        size_t attrPos = html.find("data-lyrics-container=\"true\"", searchPos);
        if (attrPos == std::string::npos) break;

        size_t openTagEnd = html.find('>', attrPos);
        if (openTagEnd == std::string::npos) break;

        // Находим закрывающий </div> с учётом вложенных <div>...</div> внутри контейнера Genius
        int depth = 1;
        size_t cur = openTagEnd + 1;
        while (cur < html.size() && depth > 0) {
            size_t nextLt = html.find('<', cur);
            if (nextLt == std::string::npos) break;

            if (html.compare(nextLt, 4, "<div") == 0 &&
                (nextLt + 4 < html.size() && (html[nextLt + 4] == ' ' || html[nextLt + 4] == '>'))) {
                depth++;
                cur = nextLt + 4;
            } else if (html.compare(nextLt, 6, "</div>") == 0) {
                depth--;
                if (depth == 0) {
                    std::string chunk = html.substr(openTagEnd + 1, nextLt - (openTagEnd + 1));
                    if (!combined.empty()) combined += "\n";
                    combined += chunk;
                    searchPos = nextLt + 6;
                    break;
                }
                cur = nextLt + 6;
            } else {
                cur = nextLt + 1;
            }
        }
        if (depth > 0) break;
    }

    if (combined.empty()) return "";

    // Заменяем <br> и <br/> на переносы строк и удаляем все остальные HTML-теги
    std::string noTags;
    noTags.reserve(combined.size());
    for (size_t i = 0; i < combined.size();) {
        if (combined[i] == '<') {
            size_t closeGt = combined.find('>', i);
            if (closeGt == std::string::npos) break;
            std::string tag = combined.substr(i + 1, closeGt - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tag.rfind("br", 0) == 0) {
                noTags.push_back('\n');
            }
            i = closeGt + 1;
        } else {
            noTags.push_back(combined[i++]);
        }
    }

    replaceAllInPlace(noTags, "&amp;", "&");
    replaceAllInPlace(noTags, "&quot;", "\"");
    replaceAllInPlace(noTags, "&#x27;", "'");
    replaceAllInPlace(noTags, "&#39;", "'");
    replaceAllInPlace(noTags, "&apos;", "'");
    replaceAllInPlace(noTags, "&lt;", "<");
    replaceAllInPlace(noTags, "&gt;", ">");
    replaceAllInPlace(noTags, "&nbsp;", " ");
    replaceAllInPlace(noTags, "\r", "");

    return trimCopy(noTags);
}

// Парсит LRC-строки вида [mm:ss.xx] для режима караоке или разбивает обычный текст по строкам
LyricsResult LyricsFetcher::parseRawLyrics(const std::string& rawText, const std::string& sourceName) {
    LyricsResult res;
    std::string cleaned = trimCopy(rawText);
    if (cleaned.empty()) {
        res.status = LyricsStatus::NotFound;
        return res;
    }

    std::istringstream ss(cleaned);
    std::string line;
    std::vector<LyricLine> syncedLines;
    std::vector<LyricLine> plainLines;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Проверяем наличие таймкода [mm:ss.xx] или [mm:ss] в начале строки
        size_t pos = 0;
        std::vector<float> timestamps;
        while (pos < line.size() && line[pos] == '[') {
            size_t closeBracket = line.find(']', pos + 1);
            if (closeBracket == std::string::npos || closeBracket - pos > 12) break;

            std::string tag = line.substr(pos + 1, closeBracket - pos - 1);
            size_t colon = tag.find(':');
            if (colon != std::string::npos) {
                bool validDigits = true;
                for (size_t k = 0; k < colon; ++k) {
                    if (!std::isdigit(static_cast<unsigned char>(tag[k]))) validDigits = false;
                }
                if (validDigits && colon > 0) {
                    try {
                        int mins = std::stoi(tag.substr(0, colon));
                        std::string secPart = tag.substr(colon + 1);
                        std::replace(secPart.begin(), secPart.end(), ':', '.');
                        float secs = std::stof(secPart);
                        timestamps.push_back(static_cast<float>(mins) * 60.0f + secs);
                        pos = closeBracket + 1;
                        continue;
                    } catch (...) {}
                }
            }
            break;
        }

        if (!timestamps.empty()) {
            std::string lyricText = trimCopy(line.substr(pos));
            if (lyricText.empty()) {
                lyricText = "♪   ♪   ♪";
            }
            for (float ts : timestamps) {
                syncedLines.push_back({ts, lyricText});
            }
        } else {
            // Пропускаем служебные мета-теги LRC вроде [ar:...], [ti:...], [by:...]
            if (line.size() > 4 && line.front() == '[' && line.back() == ']' && line.find(':') != std::string::npos) {
                continue;
            }
            plainLines.push_back({-1.0f, trimCopy(line)});
        }
    }

    if (syncedLines.size() >= 3) {
        std::sort(syncedLines.begin(), syncedLines.end(), [](const LyricLine& a, const LyricLine& b) {
            return a.timeSec < b.timeSec;
        });
        res.status = LyricsStatus::FoundSynced;
        res.sourceBadge = "KARAOKE • " + sourceName;
        res.lines = std::move(syncedLines);
        std::ostringstream oss;
        for (const auto& l : res.lines) oss << l.text << "\n";
        res.plainText = oss.str();
    } else {
        res.status = LyricsStatus::FoundPlain;
        res.sourceBadge = "TEXT • " + sourceName;
        res.plainText = cleaned;
        res.lines = std::move(plainLines);
    }

    return res;
}

std::string LyricsFetcher::computePathHash(const std::wstring& path) {
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t wc : path) {
        hash ^= static_cast<uint64_t>(wc);
        hash *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

fs::path LyricsFetcher::getLyricsCacheDir() {
    wchar_t exePath[MAX_PATH] = {0};
    fs::path baseDir;
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        baseDir = fs::path(exePath).parent_path();
    } else {
        baseDir = fs::current_path();
    }
    fs::path cacheDir = baseDir / L"lyrics_cache";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    return cacheDir;
}

bool LyricsFetcher::saveToDiskCache(const std::wstring& widePath, const LyricsResult& result) {
    if (widePath.empty() || (result.status != LyricsStatus::FoundSynced && result.status != LyricsStatus::FoundPlain)) {
        return false;
    }
    try {
        fs::path cachePath = getLyricsCacheDir() / (computePathHash(widePath) + ".lrc");
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;

        out << "[by:JewelBoxPlayer]\n";
        out << "[source:" << (result.sourceBadge.empty() ? "CACHE" : result.sourceBadge) << "]\n";

        if (result.status == LyricsStatus::FoundSynced && !result.lines.empty()) {
            for (const auto& l : result.lines) {
                if (l.timeSec >= 0.0f) {
                    int m = static_cast<int>(l.timeSec) / 60;
                    float s = l.timeSec - static_cast<float>(m * 60);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "[%02d:%05.2f]", m, s);
                    out << buf << " " << l.text << "\n";
                } else {
                    out << l.text << "\n";
                }
            }
        } else {
            out << "[type:plain]\n";
            out << result.plainText << "\n";
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool LyricsFetcher::loadFromDiskCache(const std::wstring& widePath, LyricsResult& outResult) {
    if (widePath.empty()) return false;
    try {
        fs::path cachePath = getLyricsCacheDir() / (computePathHash(widePath) + ".lrc");
        std::error_code ec;
        if (!fs::exists(cachePath, ec)) return false;

        std::ifstream in(cachePath, std::ios::binary);
        if (!in.is_open()) return false;

        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
        if (content.empty()) return false;

        std::string source = "CACHE";
        size_t sPos = content.find("[source:");
        if (sPos != std::string::npos) {
            size_t sEnd = content.find(']', sPos + 8);
            if (sEnd != std::string::npos) {
                source = content.substr(sPos + 8, sEnd - (sPos + 8));
            }
        }

        outResult = parseRawLyrics(content, source);
        return (outResult.status == LyricsStatus::FoundSynced || outResult.status == LyricsStatus::FoundPlain);
    } catch (...) {
        return false;
    }
}

LyricsFetcher::LyricsFetcher() = default;

LyricsFetcher::~LyricsFetcher() {
    m_requestId.fetch_add(1);
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

LyricsResult LyricsFetcher::getLyrics(const std::wstring& widePath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(widePath);
    if (it != m_cache.end()) {
        return it->second;
    }
    return LyricsResult{};
}

void LyricsFetcher::requestLyrics(const std::wstring& widePath,
                                  const std::string& title,
                                  const std::string& artist,
                                  int durationSec,
                                  const std::string& embeddedLyrics) {
    if (widePath.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(widePath);
        if (it != m_cache.end()) {
            if (it->second.status == LyricsStatus::FoundSynced) {
                return;
            }
            if (it->second.status == LyricsStatus::Loading) {
                return;
            }
            if (it->second.status == LyricsStatus::FoundPlain) {
                if (it->second.durationSearched) {
                    return;
                }
                // Начинаем фоновую проверку на наличие караоке, сохраняя видимый текст
                it->second.durationSearched = true;
            } else if (it->second.status == LyricsStatus::NotFound) {
                if (durationSec > 0 && !it->second.durationSearched) {
                    it->second.durationSearched = true;
                    it->second.status = LyricsStatus::Loading;
                    it->second.sourceBadge = "ПОИСК КАРАОКЕ...";
                } else {
                    return;
                }
            }
        } else {
            LyricsResult diskRes;
            if (loadFromDiskCache(widePath, diskRes)) {
                if (diskRes.status == LyricsStatus::FoundSynced) {
                    diskRes.durationSearched = true;
                    m_cache[widePath] = diskRes;
                    return;
                } else if (diskRes.status == LyricsStatus::FoundPlain) {
                    // Кладём в память для мгновенного отображения, но помечаем durationSearched = true,
                    // и всё равно запускаем фоновый поиск караоке
                    diskRes.durationSearched = true;
                    m_cache[widePath] = diskRes;
                }
            } else {
                LyricsResult loadingState;
                loadingState.status = LyricsStatus::Loading;
                loadingState.sourceBadge = "ПОИСК ТЕКСТА...";
                loadingState.durationSearched = (durationSec > 0);
                m_cache[widePath] = loadingState;
            }
        }
    }

    uint64_t newId = m_requestId.fetch_add(1) + 1;
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::thread(&LyricsFetcher::workerFunc, this, newId, widePath, title, artist, durationSec, embeddedLyrics);
}

void LyricsFetcher::forceRefresh(const std::wstring& widePath,
                                 const std::string& title,
                                 const std::string& artist,
                                 int durationSec,
                                 const std::string& embeddedLyrics) {
    if (widePath.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.erase(widePath);
    }
    try {
        fs::path cachePath = getLyricsCacheDir() / (computePathHash(widePath) + ".lrc");
        std::error_code ec;
        fs::remove(cachePath, ec);
    } catch (...) {}

    requestLyrics(widePath, title, artist, durationSec, embeddedLyrics);
}

void LyricsFetcher::workerFunc(uint64_t reqId,
                               std::wstring widePath,
                               std::string title,
                               std::string artist,
                               int durationSec,
                               std::string embeddedLyrics) {
    auto commitResult = [&](LyricsResult r) {
        r.durationSearched = (durationSec > 0);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Если у нас уже был найден обычный текст, а новый поиск вернул NotFound (караоке не найдено),
            // сохраняем отображение обычного текста, не сбрасывая его в "ТЕКСТ НЕ НАЙДЕН"
            auto it = m_cache.find(widePath);
            if (it != m_cache.end() && it->second.status == LyricsStatus::FoundPlain && r.status == LyricsStatus::NotFound) {
                it->second.durationSearched = true;
                return;
            }
            m_cache[widePath] = r;
        }
        if (r.status == LyricsStatus::FoundSynced || r.status == LyricsStatus::FoundPlain) {
            saveToDiskCache(widePath, r);
        }
    };

    // 0. Локальный .lrc файл рядом с треком (высший приоритет)
    try {
        fs::path lrcPath = fs::path(widePath);
        lrcPath.replace_extension(L".lrc");
        std::error_code ec;
        if (fs::exists(lrcPath, ec)) {
            std::ifstream f(lrcPath, std::ios::binary);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                std::string content = ss.str();
                if (!content.empty()) {
                    commitResult(parseRawLyrics(content, "LOCAL LRC"));
                    return;
                }
            }
        }
    } catch (...) {}

    // Если встроенный в ID3/FLAC текст содержит таймкоды караоке — используем сразу
    std::string fallbackPlain;
    std::string fallbackSource;
    if (!embeddedLyrics.empty()) {
        LyricsResult embRes = parseRawLyrics(embeddedLyrics, "AUDIO TAG");
        if (embRes.status == LyricsStatus::FoundSynced) {
            commitResult(embRes);
            return;
        }
        fallbackPlain = embeddedLyrics;
        fallbackSource = "AUDIO TAG";
    }

    if (m_requestId.load() != reqId) return;

    // 1. Очистка запроса
    std::string cTitle = cleanQuery(title);
    std::string cArtist = cleanQuery(artist);

    if (cArtist == "Unknown Artist" || cArtist == "Неизвестный исполнитель" || cArtist == "Server Library" || cArtist == "Cloud Stream") {
        cArtist.clear();
    }

    if (cArtist.empty()) {
        size_t dashPos = cTitle.find('-');
        if (dashPos != std::string::npos) {
            std::string p0 = cleanQuery(cTitle.substr(0, dashPos));
            std::string p1 = cleanQuery(cTitle.substr(dashPos + 1));
            if (!p0.empty() && !p1.empty()) {
                cArtist = p0;
                cTitle = p1;
            }
        }
    }

    if (cTitle.empty()) {
        if (!fallbackPlain.empty()) {
            commitResult(parseRawLyrics(fallbackPlain, fallbackSource.empty() ? "AUDIO TAG" : fallbackSource));
        } else {
            LyricsResult nf;
            nf.status = LyricsStatus::NotFound;
            commitResult(nf);
        }
        return;
    }

    std::string primaryArtist = extractPrimaryArtist(cArtist);

    // 2. Запрос к https://lrclib.net/api/get (точный поиск синхронизированного караоке)
    {
        std::string query = "/api/get?track_name=" + urlEncodeUtf8(cTitle);
        if (!cArtist.empty()) {
            query += "&artist_name=" + urlEncodeUtf8(cArtist);
        }
        if (durationSec > 0) {
            query += "&duration=" + std::to_string(durationSec);
        }

        std::string json = httpGetUtf8(L"lrclib.net", MetadataParser::utf8ToWide(query));
        if (m_requestId.load() != reqId) return;

        if (!json.empty()) {
            std::string synced = extractJsonStringField(json, "syncedLyrics");
            if (!synced.empty()) {
                commitResult(parseRawLyrics(synced, "LRCLIB"));
                return;
            }
            if (fallbackPlain.empty()) {
                std::string pl = extractJsonStringField(json, "plainLyrics");
                if (!pl.empty()) {
                    fallbackPlain = pl;
                    fallbackSource = "LRCLIB";
                }
            }
        }
    }

    // Если у трека несколько исполнителей, проверяем /api/get с primaryArtist
    if (!primaryArtist.empty() && primaryArtist != cArtist) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (m_requestId.load() != reqId) return;

        std::string query = "/api/get?track_name=" + urlEncodeUtf8(cTitle) + "&artist_name=" + urlEncodeUtf8(primaryArtist);
        if (durationSec > 0) {
            query += "&duration=" + std::to_string(durationSec);
        }

        std::string json = httpGetUtf8(L"lrclib.net", MetadataParser::utf8ToWide(query));
        if (m_requestId.load() != reqId) return;

        if (!json.empty()) {
            std::string synced = extractJsonStringField(json, "syncedLyrics");
            if (!synced.empty()) {
                commitResult(parseRawLyrics(synced, "LRCLIB"));
                return;
            }
            if (fallbackPlain.empty()) {
                std::string pl = extractJsonStringField(json, "plainLyrics");
                if (!pl.empty()) {
                    fallbackPlain = pl;
                    fallbackSource = "LRCLIB";
                }
            }
        }
    }

    // 3. Поиск караоке через https://lrclib.net/api/search со строгой приоритезацией караоке
    auto searchLrclibForSynced = [&](const std::string& queryPath) -> bool {
        if (m_requestId.load() != reqId) return false;
        std::string sJson = httpGetUtf8(L"lrclib.net", MetadataParser::utf8ToWide(queryPath));
        if (m_requestId.load() != reqId || sJson.empty()) return false;

        auto objects = extractJsonObjectsFromArray(sJson);
        if (objects.empty()) return false;

        struct Candidate {
            std::string synced;
            std::string plain;
            float score = -1.0f;
        };
        Candidate bestCandidate;

        for (const auto& obj : objects) {
            std::string synced = extractJsonStringField(obj, "syncedLyrics");
            std::string plain = extractJsonStringField(obj, "plainLyrics");
            float itemDur = extractJsonNumberField(obj, "duration");

            float score = 0.0f;
            if (!synced.empty()) {
                // Абсолютный приоритет караоке
                score = 1000.0f;
                if (durationSec > 0 && itemDur > 0.0f) {
                    float diff = std::abs(itemDur - static_cast<float>(durationSec));
                    if (diff <= 2.5f) {
                        score += 500.0f - diff * 20.0f;
                    } else if (diff <= 6.0f) {
                        score += 300.0f - diff * 10.0f;
                    } else if (diff <= 15.0f) {
                        score += 100.0f - diff * 5.0f;
                    } else {
                        score -= std::min(diff * 2.0f, 200.0f);
                    }
                }
            } else if (!plain.empty()) {
                score = 50.0f;
                if (durationSec > 0 && itemDur > 0.0f) {
                    float diff = std::abs(itemDur - static_cast<float>(durationSec));
                    if (diff <= 3.5f) score += 40.0f;
                }
            }

            if (score > bestCandidate.score) {
                bestCandidate.score = score;
                bestCandidate.synced = synced;
                bestCandidate.plain = plain;
            }
        }

        if (!bestCandidate.synced.empty()) {
            commitResult(parseRawLyrics(bestCandidate.synced, "LRCLIB"));
            return true;
        }
        if (fallbackPlain.empty() && !bestCandidate.plain.empty()) {
            fallbackPlain = bestCandidate.plain;
            fallbackSource = "LRCLIB";
        }
        return false;
    };

    // Небольшая пауза перед поиском во избежание burst rate-limit
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Сначала точный поиск по имени трека и полному исполнителю
    std::string sPath1 = "/api/search?track_name=" + urlEncodeUtf8(cTitle);
    if (!cArtist.empty()) sPath1 += "&artist_name=" + urlEncodeUtf8(cArtist);
    if (searchLrclibForSynced(sPath1)) return;

    // Затем поиск по имени трека и primaryArtist (если отличается)
    if (!primaryArtist.empty() && primaryArtist != cArtist) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::string sPathPrimary = "/api/search?track_name=" + urlEncodeUtf8(cTitle) + "&artist_name=" + urlEncodeUtf8(primaryArtist);
        if (searchLrclibForSynced(sPathPrimary)) return;
    }

    // Затем общий поиск по q=...
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    std::string searchQuery = cArtist.empty() ? cTitle : (cArtist + " " + cTitle);
    std::string sPath2 = "/api/search?q=" + urlEncodeUtf8(searchQuery);
    if (searchLrclibForSynced(sPath2)) return;

    // Затем общий поиск по q=primaryArtist + title
    if (!primaryArtist.empty() && primaryArtist != cArtist) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::string sPathPrimaryQ = "/api/search?q=" + urlEncodeUtf8(primaryArtist + " " + cTitle);
        if (searchLrclibForSynced(sPathPrimaryQ)) return;
    }

    if (m_requestId.load() != reqId) return;

    // Если караоке нигде не нашлось, но был найден обычный текст песни — сохраняем его
    if (!fallbackPlain.empty()) {
        commitResult(parseRawLyrics(fallbackPlain, fallbackSource.empty() ? "LRCLIB" : fallbackSource));
        return;
    }

    // 4. Фолбэк на Genius (plain text)
    {
        std::string gPath = "/api/search/song?q=" + urlEncodeUtf8(searchQuery) + "&page=1";
        std::wstring browserUA = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
        std::string gJson = httpGetUtf8(L"genius.com", MetadataParser::utf8ToWide(gPath), browserUA);
        if (m_requestId.load() != reqId) return;

        if (!gJson.empty()) {
            size_t searchPos = 0;
            std::string songUrl;
            while (searchPos < gJson.size()) {
                size_t nextPos = std::string::npos;
                std::string candidateUrl = extractJsonStringField(gJson, "url", searchPos, &nextPos);
                if (candidateUrl.empty() || nextPos == std::string::npos) break;
                if (candidateUrl.find("genius.com/") != std::string::npos &&
                    candidateUrl.find("-lyrics") != std::string::npos) {
                    songUrl = candidateUrl;
                    break;
                }
                searchPos = nextPos;
            }

            if (!songUrl.empty()) {
                size_t hostPos = songUrl.find("genius.com");
                if (hostPos != std::string::npos) {
                    std::string pagePath = songUrl.substr(hostPos + 10);
                    if (pagePath.empty()) pagePath = "/";
                    std::string html = httpGetUtf8(L"genius.com", MetadataParser::utf8ToWide(pagePath), browserUA);
                    if (m_requestId.load() != reqId) return;

                    std::string geniusLyrics = extractGeniusLyricsFromHtml(html);
                    if (!geniusLyrics.empty()) {
                        commitResult(parseRawLyrics(geniusLyrics, "GENIUS"));
                        return;
                    }
                }
            }
        }
    }

    LyricsResult notFound;
    notFound.status = LyricsStatus::NotFound;
    notFound.sourceBadge = "ТЕКСТ НЕ НАЙДЕН";
    commitResult(notFound);
}