#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <filesystem>

enum class LyricsStatus {
    None,
    Loading,
    FoundSynced, // Караоке с таймкодами [mm:ss.xx] (LRCLIB / LRC)
    FoundPlain,  // Обычный текст песни (Genius / LRCLIB Plain / ID3)
    NotFound
};

struct LyricLine {
    float timeSec = 0.0f;
    std::string text;
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::None;
    std::string sourceBadge; // Например: "KARAOKE • LRCLIB", "GENIUS LYRICS", "ID3 TAG"
    std::string plainText;
    std::vector<LyricLine> lines;
    bool durationSearched = false; // Был ли уже выполнен поиск с известной длительностью
};

class LyricsFetcher {
public:
    LyricsFetcher();
    ~LyricsFetcher();

    LyricsFetcher(const LyricsFetcher&) = delete;
    LyricsFetcher& operator=(const LyricsFetcher&) = delete;

    // Запускает автопоиск текста для трека (если ещё не в кэше)
    void requestLyrics(const std::wstring& widePath,
                       const std::string& title,
                       const std::string& artist,
                       int durationSec,
                       const std::string& embeddedLyrics = "");

    // Принудительный повторный поиск
    void forceRefresh(const std::wstring& widePath,
                      const std::string& title,
                      const std::string& artist,
                      int durationSec,
                      const std::string& embeddedLyrics = "");

    // Потокобезопасное получение текущего результата для пути трека
    LyricsResult getLyrics(const std::wstring& widePath) const;

    static std::string cleanQuery(const std::string& text);
    static LyricsResult parseRawLyrics(const std::string& rawText, const std::string& sourceName);

    // Персистентный дисковый кэш (lyrics_cache/)
    static std::filesystem::path getLyricsCacheDir();
    static std::string computePathHash(const std::wstring& path);
    static bool saveToDiskCache(const std::wstring& widePath, const LyricsResult& result);
    static bool loadFromDiskCache(const std::wstring& widePath, LyricsResult& outResult);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, LyricsResult> m_cache;
    std::atomic<uint64_t> m_requestId{0};
    std::thread m_worker;

    void workerFunc(uint64_t reqId,
                    std::wstring widePath,
                    std::string title,
                    std::string artist,
                    int durationSec,
                    std::string embeddedLyrics);
};