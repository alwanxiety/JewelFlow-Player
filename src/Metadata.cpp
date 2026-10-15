#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Metadata.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <cwctype>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

std::wstring MetadataParser::utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (sz <= 0) return L"";
    std::wstring res(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), res.data(), sz);
    return res;
}

std::string MetadataParser::wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return "";
    std::string res(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), res.data(), sz, nullptr, nullptr);
    return res;
}

std::wstring MetadataParser::toLowerWide(const std::wstring& str) {
    if (str.empty()) return L"";
    std::wstring res = str;
    CharLowerBuffW(res.data(), static_cast<DWORD>(res.size()));
    return res;
}

static std::string trimString(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\r\n\0");
    if (first == std::string::npos) return "";
    const auto last = str.find_last_not_of(" \t\r\n\0");
    return str.substr(first, last - first + 1);
}

static std::string sanitizeDashes(const std::string& s) {
    std::string out = s;
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.length(), to);
            pos += to.length();
        }
    };
    replaceAll("\xE2\x80\x94", "-");
    replaceAll("\xE2\x80\x93", "-");
    replaceAll("\xE2\x88\x92", "-");
    return out;
}

void MetadataParser::cleanup(TrackData& track) {
    if (track.coverTex) {
        glDeleteTextures(1, &track.coverTex);
        track.coverTex = 0;
    }
    if (track.spineTex) {
        glDeleteTextures(1, &track.spineTex);
        track.spineTex = 0;
    }
    if (track.badgeTex) {
        glDeleteTextures(1, &track.badgeTex);
        track.badgeTex = 0;
    }
}

static void applySharpTextureParams(float lodBias = -0.50f) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias);

    static float s_maxAniso = -1.0f;
    if (s_maxAniso < 0.0f) {
        s_maxAniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &s_maxAniso);
    }
    if (s_maxAniso > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (std::min)(s_maxAniso, 16.0f));
    }
}

static GLuint uploadTextureFromMemory(const unsigned char* data, int size, glm::vec3* outAccent) {
    if (!data || size <= 0) return 0;
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, ch = 0;
    unsigned char* img = stbi_load_from_memory(data, size, &w, &h, &ch, 4);
    if (!img || w <= 0 || h <= 0) {
        if (img) stbi_image_free(img);
        return 0;
    }

    if (outAccent) {
        double r = 0, g = 0, b = 0;
        double weightSum = 0.0;
        int totalPixels = w * h;
        int step = (totalPixels > 8000) ? (totalPixels / 800) : 1;
        for (int i = 0; i < totalPixels * 4; i += step * 4) {
            unsigned char pr = img[i];
            unsigned char pg = img[i + 1];
            unsigned char pb = img[i + 2];
            int maxC = (pr > pg) ? ((pr > pb) ? pr : pb) : ((pg > pb) ? pg : pb);
            int minC = (pr < pg) ? ((pr < pb) ? pr : pb) : ((pg < pb) ? pg : pb);
            int sat = maxC - minC;
            if (maxC > 40 && maxC < 245 && sat > 24) {
                double wFactor = static_cast<double>(sat);
                r += pr * wFactor;
                g += pg * wFactor;
                b += pb * wFactor;
                weightSum += wFactor;
            }
        }
        if (weightSum > 0.0) {
            glm::vec3 col(
                static_cast<float>((r / weightSum) / 255.0),
                static_cast<float>((g / weightSum) / 255.0),
                static_cast<float>((b / weightSum) / 255.0)
            );
            float maxComp = (std::max)({col.r, col.g, col.b});
            if (maxComp > 0.01f && maxComp < 0.75f) {
                col *= (0.82f / maxComp);
            }
            *outAccent = glm::clamp(col, glm::vec3(0.15f), glm::vec3(0.98f));
        }
    }

    // Если обложка слишком огромная (> 512px), быстро уменьшаем её в 2–4 раза перед отправкой в GPU,
    // чтобы не тратить время на генерацию гигантских мипмапов в Debug-режиме
    const unsigned char* uploadPtr = img;
    std::vector<unsigned char> downscaled;
    int upW = w, upH = h;
    if (w > 600 || h > 600) {
        upW = std::min(512, w);
        upH = std::min(512, h);
        downscaled.resize(static_cast<size_t>(upW * upH * 4));
        unsigned char* dst = downscaled.data();
        for (int y = 0; y < upH; ++y) {
            int sy = (y * h) / upH;
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(img + sy * w * 4);
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(dst + y * upW * 4);
            for (int x = 0; x < upW; ++x) {
                dstRow[x] = srcRow[(x * w) / upW];
            }
        }
        uploadPtr = downscaled.data();
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, upW, upH, 0, GL_RGBA, GL_UNSIGNED_BYTE, uploadPtr);
    glGenerateMipmap(GL_TEXTURE_2D);
    applySharpTextureParams(-0.45f);

    stbi_image_free(img);
    return tex;
}

static GLuint loadTextureFromFile(const fs::path& p, glm::vec3* outAccent) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return 0;
    std::streamsize sz = f.tellg();
    if (sz <= 0 || sz > 32 * 1024 * 1024) return 0;
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) return 0;
    return uploadTextureFromMemory(buf.data(), static_cast<int>(sz), outAccent);
}

static std::string decodeId3Text(const uint8_t* data, size_t size) {
    if (!data || size < 2) return "";
    uint8_t encoding = data[0];
    const uint8_t* payload = data + 1;
    size_t len = size - 1;

    if (encoding == 0) {
        int wlen = MultiByteToWideChar(1251, 0, reinterpret_cast<const char*>(payload), static_cast<int>(len), nullptr, 0);
        if (wlen <= 0) return "";
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(1251, 0, reinterpret_cast<const char*>(payload), static_cast<int>(len), wstr.data(), wlen);
        if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
        return sanitizeDashes(trimString(MetadataParser::wideToUtf8(wstr)));
    } else if (encoding == 1 || encoding == 2) {
        if (len < 2) return "";
        bool bigEndian = (encoding == 2);
        size_t offset = 0;
        if (encoding == 1) {
            if (payload[0] == 0xFE && payload[1] == 0xFF) {
                bigEndian = true;
                offset = 2;
            } else if (payload[0] == 0xFF && payload[1] == 0xFE) {
                bigEndian = false;
                offset = 2;
            }
        }
        size_t charCount = (len - offset) / 2;
        std::wstring wstr;
        wstr.reserve(charCount);
        for (size_t i = 0; i < charCount; ++i) {
            uint8_t b0 = payload[offset + i * 2];
            uint8_t b1 = payload[offset + i * 2 + 1];
            wchar_t ch = bigEndian ? static_cast<wchar_t>((b0 << 8) | b1)
                                   : static_cast<wchar_t>((b1 << 8) | b0);
            if (ch == L'\0') break;
            wstr.push_back(ch);
        }
        return sanitizeDashes(trimString(MetadataParser::wideToUtf8(wstr)));
    } else if (encoding == 3) {
        std::string s(reinterpret_cast<const char*>(payload), len);
        auto nullPos = s.find('\0');
        if (nullPos != std::string::npos) s.resize(nullPos);
        return sanitizeDashes(trimString(s));
    }
    return "";
}

static uint32_t readSyncSafe32(const uint8_t* b) {
    return ((b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14) | ((b[2] & 0x7F) << 7) | (b[3] & 0x7F);
}

static uint32_t readBE32(const uint8_t* b) {
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8)  |
            static_cast<uint32_t>(b[3]);
}

static GLuint parseAudioTagsAndCover(const fs::path& path, TrackData& track) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return 0;

    uint8_t header[10] = {0};
    if (!file.read(reinterpret_cast<char*>(header), 10)) return 0;

    if (std::memcmp(header, "ID3", 3) == 0) {
        uint8_t version = header[3];
        uint8_t flags = header[5];
        uint32_t tagSize = readSyncSafe32(header + 6);
        if (tagSize > 0 && tagSize < 16 * 1024 * 1024) {
            std::vector<uint8_t> tagData(tagSize);
            if (file.read(reinterpret_cast<char*>(tagData.data()), tagSize)) {
                size_t pos = 0;
                if ((flags & 0x40) && tagSize >= 4) {
                    uint32_t extSize = (version == 4) ? readSyncSafe32(tagData.data()) : readBE32(tagData.data());
                    pos += (version == 4) ? extSize : (extSize + 4);
                }

                GLuint coverTex = 0;
                while (pos + 10 <= tagSize) {
                    if (tagData[pos] == 0) break;
                    char frameId[5] = {
                        static_cast<char>(tagData[pos]),
                        static_cast<char>(tagData[pos + 1]),
                        static_cast<char>(tagData[pos + 2]),
                        static_cast<char>(tagData[pos + 3]),
                        '\0'
                    };
                    uint32_t frameSize = (version == 4)
                        ? readSyncSafe32(tagData.data() + pos + 4)
                        : readBE32(tagData.data() + pos + 4);
                    pos += 10;

                    if (frameSize == 0 || pos + frameSize > tagSize) break;
                    const uint8_t* fData = tagData.data() + pos;

                    if (std::strcmp(frameId, "TIT2") == 0) {
                        std::string val = decodeId3Text(fData, frameSize);
                        if (!val.empty()) track.title = val;
                    } else if (std::strcmp(frameId, "TPE1") == 0) {
                        std::string val = decodeId3Text(fData, frameSize);
                        if (!val.empty()) track.artist = val;
                    } else if (std::strcmp(frameId, "TALB") == 0) {
                        std::string val = decodeId3Text(fData, frameSize);
                        if (!val.empty()) track.album = val;
                    } else if (std::strcmp(frameId, "APIC") == 0 && coverTex == 0 && frameSize > 16) {
                        uint8_t enc = fData[0];
                        size_t idx = 1;
                        while (idx < frameSize && fData[idx] != 0) idx++;
                        idx++;
                        if (idx < frameSize) {
                            idx++;
                            if (enc == 1 || enc == 2) {
                                while (idx + 1 < frameSize && !(fData[idx] == 0 && fData[idx + 1] == 0)) idx += 2;
                                idx += 2;
                            } else {
                                while (idx < frameSize && fData[idx] != 0) idx++;
                                idx++;
                            }
                            if (idx < frameSize) {
                                coverTex = uploadTextureFromMemory(fData + idx, static_cast<int>(frameSize - idx), &track.accentColor);
                            }
                        }
                    }
                    pos += frameSize;
                }
                return coverTex;
            }
        }
    }

    if (std::memcmp(header, "fLaC", 4) == 0) {
        file.seekg(4, std::ios::beg);
        bool isLast = false;
        GLuint coverTex = 0;
        while (!isLast && file.good()) {
            uint8_t bh[4] = {0};
            if (!file.read(reinterpret_cast<char*>(bh), 4)) break;
            isLast = (bh[0] & 0x80) != 0;
            uint8_t blockType = bh[0] & 0x7F;
            uint32_t blockLen = (static_cast<uint32_t>(bh[1]) << 16) |
                                (static_cast<uint32_t>(bh[2]) << 8)  |
                                 static_cast<uint32_t>(bh[3]);

            if (blockLen == 0 || blockLen > 16 * 1024 * 1024) {
                file.seekg(blockLen, std::ios::cur);
                continue;
            }

            if (blockType == 4) {
                std::vector<uint8_t> buf(blockLen);
                if (file.read(reinterpret_cast<char*>(buf.data()), blockLen) && blockLen >= 8) {
                    auto readLE32 = [](const uint8_t* p) -> uint32_t {
                        return static_cast<uint32_t>(p[0]) |
                              (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24);
                    };
                    size_t off = 0;
                    uint32_t vendorLen = readLE32(buf.data());
                    off += 4 + vendorLen;
                    if (off + 4 <= blockLen) {
                        uint32_t count = readLE32(buf.data() + off);
                        off += 4;
                        for (uint32_t i = 0; i < count && off + 4 <= blockLen; ++i) {
                            uint32_t clen = readLE32(buf.data() + off);
                            off += 4;
                            if (off + clen > blockLen) break;
                            std::string comment(reinterpret_cast<const char*>(buf.data() + off), clen);
                            off += clen;
                            auto eq = comment.find('=');
                            if (eq != std::string::npos) {
                                std::string key = comment.substr(0, eq);
                                std::string val = sanitizeDashes(trimString(comment.substr(eq + 1)));
                                std::transform(key.begin(), key.end(), key.begin(), ::toupper);
                                if (!val.empty()) {
                                    if (key == "TITLE") track.title = val;
                                    else if (key == "ARTIST") track.artist = val;
                                    else if (key == "ALBUM") track.album = val;
                                }
                            }
                        }
                    }
                }
            } else if (blockType == 6 && coverTex == 0) {
                std::vector<uint8_t> buf(blockLen);
                if (file.read(reinterpret_cast<char*>(buf.data()), blockLen) && blockLen > 32) {
                    size_t off = 4;
                    if (off + 4 <= blockLen) {
                        uint32_t mimeLen = readBE32(buf.data() + off);
                        off += 4 + mimeLen;
                        if (off + 4 <= blockLen) {
                            uint32_t descLen = readBE32(buf.data() + off);
                            off += 4 + descLen + 16;
                            if (off + 4 <= blockLen) {
                                uint32_t dataLen = readBE32(buf.data() + off);
                                off += 4;
                                if (off + dataLen <= blockLen && dataLen > 0) {
                                    coverTex = uploadTextureFromMemory(buf.data() + off, static_cast<int>(dataLen), &track.accentColor);
                                }
                            }
                        }
                    }
                }
            } else {
                file.seekg(blockLen, std::ios::cur);
            }
        }
        return coverTex;
    }

    return 0;
}

// Сверхбыстрая генерация плашки обложки (512x168) на чистой целочисленной арифметике (выполняется за ~1 мс даже в Debug)
static GLuint createCoverBottomBadgeGDI(const std::wstring& artist, const std::wstring& title, const glm::vec3& accent) {
    const int W = 512, H = 168;
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* textBits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &textBits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT fullRect = {0, 0, W, H};
    FillRect(memDC, &fullRect, blackBrush);
    DeleteObject(blackBrush);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));

    LOGFONTW lfTitle = {0};
    lfTitle.lfHeight = 40;
    lfTitle.lfWeight = FW_EXTRABOLD;
    lfTitle.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lfTitle.lfFaceName, L"Segoe UI");

    HFONT hFontTitle = CreateFontIndirectW(&lfTitle);
    HGDIOBJ oldFont = SelectObject(memDC, hFontTitle);

    RECT trcTitle = {28, 34, W - 28, 86};
    DrawTextW(memDC, title.c_str(), -1, &trcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    LOGFONTW lfArtist = {0};
    lfArtist.lfHeight = 26;
    lfArtist.lfWeight = FW_SEMIBOLD;
    lfArtist.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lfArtist.lfFaceName, L"Segoe UI");

    HFONT hFontArtist = CreateFontIndirectW(&lfArtist);
    SelectObject(memDC, hFontArtist);

    RECT trcArtist = {28, 88, W - 28, 128};
    DrawTextW(memDC, artist.c_str(), -1, &trcArtist, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    GdiFlush();

    std::vector<unsigned char> finalImg(W * H * 4);
    unsigned char* dstBase = finalImg.data();
    const unsigned char* maskRaw = static_cast<const unsigned char*>(textBits);

    uint8_t accR = static_cast<uint8_t>(std::clamp(accent.r * 255.0f, 0.0f, 255.0f));
    uint8_t accG = static_cast<uint8_t>(std::clamp(accent.g * 255.0f, 0.0f, 255.0f));
    uint8_t accB = static_cast<uint8_t>(std::clamp(accent.b * 255.0f, 0.0f, 255.0f));

    for (int y = 0; y < H; ++y) {
        int srcY = H - 1 - y;
        // Быстрая целочисленная аппроксимация градиента прозрачности (0..240)
        int gradA = (srcY * 242) / (H - 1);
        uint32_t textTintR = (srcY >= 86) ? 215 : 255;
        uint32_t textTintG = (srcY >= 86) ? 222 : 255;
        uint32_t textTintB = (srcY >= 86) ? 235 : 255;
        bool inBarRow = (srcY >= 136 && srcY <= 141);

        const unsigned char* srcRow = maskRaw + srcY * W * 4;
        unsigned char* dstRow = dstBase + y * W * 4;

        for (int x = 0; x < W; ++x) {
            if (inBarRow && x >= 28 && x <= 132) {
                dstRow[x * 4 + 0] = accR;
                dstRow[x * 4 + 1] = accG;
                dstRow[x * 4 + 2] = accB;
                dstRow[x * 4 + 3] = 255;
            } else {
                uint32_t tCov = srcRow[x * 4 + 1]; // Зелёный канал несёт основную яркость ClearType
                uint32_t invCov = 255 - tCov;
                dstRow[x * 4 + 0] = static_cast<unsigned char>((10 * invCov + textTintR * tCov) >> 8);
                dstRow[x * 4 + 1] = static_cast<unsigned char>((12 * invCov + textTintG * tCov) >> 8);
                dstRow[x * 4 + 2] = static_cast<unsigned char>((18 * invCov + textTintB * tCov) >> 8);
                dstRow[x * 4 + 3] = static_cast<unsigned char>((gradA > static_cast<int>(tCov)) ? gradA : tCov);
            }
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, dstBase);
    glGenerateMipmap(GL_TEXTURE_2D);
    applySharpTextureParams(-0.55f);

    SelectObject(memDC, oldFont);
    DeleteObject(hFontTitle);
    DeleteObject(hFontArtist);
    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    return tex;
}

// Сверхбыстрая генерация дефолтной обложки (256x256) без тяжёлых тригонометрических вызовов
static GLuint createFallbackCover(const glm::vec3& accent) {
    const int S = 256;
    std::vector<unsigned char> img(S * S * 4);
    unsigned char* dst = img.data();

    int ar = static_cast<int>(accent.r * 215.0f);
    int ag = static_cast<int>(accent.g * 215.0f);
    int ab = static_cast<int>(accent.b * 215.0f);

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            int t = (y * 190 + (S - 1 - x) * 65) >> 8;
            int invT = 255 - t;
            int r = (20 * invT + ar * t) >> 8;
            int g = (23 * invT + ag * t) >> 8;
            int b = (33 * invT + ab * t) >> 8;

            int dx = x - 128;
            int dy = y - 132;
            int d2 = dx * dx + dy * dy;
            if (d2 >= 100 && d2 <= 625) {
                r = static_cast<int>(accent.r * 250.0f);
                g = static_cast<int>(accent.g * 250.0f);
                b = static_cast<int>(accent.b * 250.0f);
            } else if (d2 < 100) {
                r = 18; g = 20; b = 26;
            }

            int idx = (y * S + x) * 4;
            dst[idx + 0] = static_cast<unsigned char>(r);
            dst[idx + 1] = static_cast<unsigned char>(g);
            dst[idx + 2] = static_cast<unsigned char>(b);
            dst[idx + 3] = 255;
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    glGenerateMipmap(GL_TEXTURE_2D);
    applySharpTextureParams(-0.4f);

    return tex;
}

TrackData MetadataParser::parse(const std::wstring& path, int index) {
    TrackData t;
    t.widePath = path;
    t.filePath = wideToUtf8(path);

    fs::path p(path);
    std::wstring stem = p.stem().wstring();
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towupper);
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    t.formatBadge = wideToUtf8(ext.empty() ? L"AUDIO" : ext);

    std::string rawStem = sanitizeDashes(wideToUtf8(stem));
    t.title = trimString(rawStem);
    t.artist = "Unknown Artist";

    size_t dashPos = rawStem.find(" - ");
    if (dashPos != std::string::npos) {
        std::string candArtist = trimString(rawStem.substr(0, dashPos));
        std::string candTitle = trimString(rawStem.substr(dashPos + 3));
        if (!candArtist.empty() && !candTitle.empty()) {
            t.artist = candArtist;
            t.title = candTitle;
        }
    } else if ((dashPos = rawStem.find('-')) != std::string::npos) {
        std::string candArtist = trimString(rawStem.substr(0, dashPos));
        std::string candTitle = trimString(rawStem.substr(dashPos + 1));
        if (!candArtist.empty() && !candTitle.empty()) {
            t.artist = candArtist;
            t.title = candTitle;
        }
    }

    static const glm::vec3 fallbackColors[] = {
        {0.95f, 0.26f, 0.42f},
        {0.22f, 0.58f, 0.96f},
        {0.18f, 0.78f, 0.56f},
        {0.96f, 0.62f, 0.18f},
        {0.66f, 0.38f, 0.94f}
    };
    t.accentColor = fallbackColors[index % 5];

    t.coverTex = parseAudioTagsAndCover(p, t);

    if (t.coverTex == 0) {
        std::error_code ec;
        fs::path dir = p.parent_path();
        const std::wstring names[] = {
            L"cover.jpg", L"cover.png", L"folder.jpg", L"folder.png",
            L"album.jpg", L"album.png", L"front.jpg", L"front.png"
        };
        for (const auto& n : names) {
            if (fs::exists(dir / n, ec)) {
                t.coverTex = loadTextureFromFile(dir / n, &t.accentColor);
                if (t.coverTex != 0) break;
            }
        }
    }

    std::wstring wArtist = utf8ToWide(t.artist);
    std::wstring wTitle = utf8ToWide(t.title);
    std::wstring wAlbum = utf8ToWide(t.album);

    if (t.coverTex == 0) {
        t.coverTex = createFallbackCover(t.accentColor);
    }

    t.spineTex = 0;
    t.badgeTex = createCoverBottomBadgeGDI(wArtist, wTitle, t.accentColor);
    t.searchKeyLower = toLowerWide(wTitle + L" " + wArtist + L" " + wAlbum);

    return t;
}