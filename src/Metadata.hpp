#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

struct TrackData {
    std::wstring widePath;
    std::string filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::string formatBadge;
    std::wstring searchKeyLower;
    glm::vec3 accentColor{0.96f, 0.22f, 0.38f};
    GLuint coverTex = 0;
    GLuint spineTex = 0;
    GLuint badgeTex = 0;
};

class MetadataParser {
public:
    static TrackData parse(const std::wstring& path, int index);
    static void cleanup(TrackData& track);
    static std::wstring toLowerWide(const std::wstring& str);
    static std::wstring utf8ToWide(const std::string& utf8);
    static std::string wideToUtf8(const std::wstring& wide);
};
