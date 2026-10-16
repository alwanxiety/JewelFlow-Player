#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_set>
#include <deque>
#include <filesystem>
#include <cstdint>

struct CoverResult {
    std::wstring widePath;
    std::vector<uint8_t> imageData;
};

class CoverFetcher {
public:
    CoverFetcher();
    ~CoverFetcher();

    CoverFetcher(const CoverFetcher&) = delete;
    CoverFetcher& operator=(const CoverFetcher&) = delete;

    // Запрос автопоиска обложки для трека в фоновом потоке
    void requestCover(const std::wstring& widePath,
                      const std::string& artist,
                      const std::string& title,
                      const std::string& album,
                      bool forceRefresh = false);

    // Извлечь все завершённые скачивания обложек (для загрузки в OpenGL в главном потоке)
    std::vector<CoverResult> pollCompleted();

    // Дисковый кэш обложек (covers_cache/)
    static std::filesystem::path getCoverCacheDir();
    static std::filesystem::path getCachedCoverPath(const std::string& artist, const std::string& title);
    static std::string computeTrackCoverHash(const std::string& artist, const std::string& title);

private:
    struct RequestItem {
        std::wstring widePath;
        std::string artist;
        std::string title;
        std::string album;
        bool forceRefresh = false;
    };

    void workerLoop();
    bool fetchCoverOnline(const RequestItem& req, std::vector<uint8_t>& outImage);
    bool searchItunes(const std::string& query, std::string& outArtworkUrl);
    bool searchDeezer(const std::string& query, std::string& outArtworkUrl);
    bool downloadBinaryFromUrl(const std::string& url, std::vector<uint8_t>& outData);

    std::atomic<bool> m_running{false};
    std::thread m_workerThread;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::deque<RequestItem> m_queue;
    std::unordered_set<std::wstring> m_inFlight;
    std::vector<CoverResult> m_completed;
};
