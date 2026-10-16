#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <filesystem>

struct YmTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    int duration = 0;
    std::string coverUrl;
    std::string thumbPath;
};

struct YmCompletedDownload {
    std::string trackId;
    std::wstring localPath;
    std::wstring coverPath;
    std::string title;
    std::string artist;
    std::string album;
    int duration = 0;
};

class YandexMusicManager {
public:
    static YandexMusicManager& instance();

    void init(const std::filesystem::path& exeDir);
    void shutdown();

    // Статус авторизации
    bool isAuthenticated() const;
    std::string getUserName() const;
    bool isBridgeRunning() const;

    void refreshAuthStatus();
    void submitTokenAsync(const std::string& tokenOrUrl, std::function<void(bool success, const std::string& msg)> onDone = nullptr);
    void logoutAsync(std::function<void()> onDone = nullptr);

    // Онлайн-поиск
    void searchAsync(const std::string& query);
    bool isSearching() const;
    std::vector<YmTrack> getSearchResults();
    void clearSearchResults();

    // Скачивание трека
    void downloadTrackAsync(const YmTrack& track);
    bool isTrackDownloading(const std::string& trackId) const;
    std::vector<YmCompletedDownload> pollCompletedDownloads();

    // Статус последнего действия (для отображения в UI)
    std::string getAuthMessage() const;
    bool isAuthBusy() const;

private:
    YandexMusicManager();
    ~YandexMusicManager();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
