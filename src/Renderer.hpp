#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "Metadata.hpp"

enum class AAMode {
    FXAA = 0, // Быстрое экранное сглаживание (~0.8-1% GPU, высокая чёткость, максимальный FPS)
    MSAA = 1, // Аппаратное мультисэмпловое сглаживание 2x MSAA
    Off = 2   // Без сглаживания
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init();
    void resize(int width, int height);

    void setAAMode(AAMode mode) { m_aaMode = mode; }
    AAMode getAAMode() const { return m_aaMode; }

    void setReflectionsEnabled(bool enabled) { m_reflectionsEnabled = enabled; }
    bool isReflectionsEnabled() const { return m_reflectionsEnabled; }

    void renderShelf(const std::vector<TrackData>& playlist,
                     const std::vector<int>& visibleIndices,
                     const std::vector<float>& hoverWeights,
                     float shelfScroll,
                     int selectedIndex,
                     int activePlayingIndex,
                     int rouletteHighlightIndex,
                     float discFlightProgress,
                     float inspectProgress,
                     float inspectFrontFlip,
                     float pulseTime,
                     const glm::vec3& themeAccent);

    int pickTrack(double mouseX,
                  double mouseY,
                  const std::vector<int>& visibleIndices,
                  const std::vector<float>& hoverWeights,
                  float shelfScroll) const;

    glm::vec2 getBoxBottomScreenPos(int slotIndex,
                                    float hoverWeight,
                                    float shelfScroll) const;

    glm::vec2 getBoxTopScreenPos(int slotIndex,
                                 float hoverWeight,
                                 float shelfScroll) const;

    // Возвращает экранные координаты (minX, minY, maxX, maxY) задней крышки развёрнутого 3D-бокса
    glm::vec4 getInspectBoxScreenRect() const;

    // Статус и текущий цвет плавного заполнения фона акцентом
    bool isAccentTransitioning() const { return m_accentTransitionProg < 1.0f; }
    glm::vec3 getCurrentAccent() const;
    glm::vec3 getAccentAtScreenPos(const glm::vec2& screenPos) const;

private:
    AAMode m_aaMode = AAMode::MSAA;
    bool m_reflectionsEnabled = true;

    GLuint m_bgShader = 0;
    GLuint m_caseShader = 0;
    GLuint m_shadowShader = 0;
    GLuint m_fxaaShader = 0;
    GLuint m_copyShader = 0;

    GLint m_bgLocAccent = -1;
    GLint m_bgLocPrevAccent = -1;
    GLint m_bgLocTransition = -1;
    GLint m_bgLocOrigin = -1;
    GLint m_bgLocAspect = -1;
    GLint m_bgLocTime = -1;

    // Плавное заполнение сцены акцентным цветом при переключении треков
    glm::vec3 m_prevAccent{0.95f, 0.26f, 0.42f};
    glm::vec3 m_targetAccent{0.95f, 0.26f, 0.42f};
    float m_accentTransitionProg = 1.0f;
    glm::vec2 m_accentOrigin{0.5f, 0.45f};
    int m_lastAccentTrack = -999;
    float m_lastPulseTime = 0.0f;
    bool m_firstAccentFrame = true;

    GLint m_caseLocModel = -1;
    GLint m_caseLocView = -1;
    GLint m_caseLocProj = -1;
    GLint m_caseLocCamPos = -1;
    GLint m_caseLocBaseColor = -1;
    GLint m_caseLocAccent = -1;
    GLint m_caseLocType = -1;
    GLint m_caseLocTex = -1;
    GLint m_caseLocOpacity = -1;
    GLint m_caseLocHighlight = -1;

    GLint m_shLocModel = -1;
    GLint m_shLocView = -1;
    GLint m_shLocProj = -1;
    GLint m_shLocColor = -1;
    GLint m_shLocLift = -1;

    GLint m_fxaaLocTex = -1;
    GLint m_fxaaLocRcpFrame = -1;

    GLint m_copyLocTex = -1;

    GLuint m_bgVAO = 0, m_bgVBO = 0;
    GLuint m_boxVAO = 0, m_boxVBO = 0, m_boxEBO = 0;
    GLuint m_shadowVAO = 0, m_shadowVBO = 0;
    GLuint m_badgeVAO = 0, m_badgeVBO = 0;
    GLuint m_reflVAO = 0, m_reflVBO = 0, m_reflEBO = 0;

    int m_width = 0;
    int m_height = 0;
    glm::mat4 m_proj{1.0f};
    glm::mat4 m_view{1.0f};
    glm::vec3 m_camPos{0.0f, 0.12f, 5.6f};

    // Односэмпловый FBO (для FXAA и режима Off)
    GLuint m_sceneFBO = 0;
    GLuint m_sceneColorTex = 0;
    GLuint m_sceneDepthRB = 0;

    // 2x MSAA FBO (для аппаратного режима)
    GLuint m_msaaFBO = 0;
    GLuint m_msaaColorRB = 0;
    GLuint m_msaaDepthRB = 0;
    int m_samples = 2;

    void buildGeometry();
    void compileShaders();
};