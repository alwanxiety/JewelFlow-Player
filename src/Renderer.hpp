#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "Metadata.hpp"

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init();
    void resize(int width, int height);

    void renderShelf(const std::vector<TrackData>& playlist,
                     const std::vector<int>& visibleIndices,
                     const std::vector<float>& hoverWeights,
                     float shelfScroll,
                     int selectedIndex,
                     int activePlayingIndex,
                     int rouletteHighlightIndex,
                     float discFlightProgress,
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

private:
    GLuint m_bgShader = 0;
    GLuint m_caseShader = 0;
    GLuint m_shadowShader = 0;

    GLint m_bgLocAccent = -1;
    GLint m_bgLocTime = -1;

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

    GLuint m_bgVAO = 0, m_bgVBO = 0;
    GLuint m_boxVAO = 0, m_boxVBO = 0, m_boxEBO = 0;
    GLuint m_shadowVAO = 0, m_shadowVBO = 0;
    GLuint m_badgeVAO = 0, m_badgeVBO = 0;

    int m_width = 0;
    int m_height = 0;
    glm::mat4 m_proj{1.0f};
    glm::mat4 m_view{1.0f};
    glm::vec3 m_camPos{0.0f, 0.12f, 5.6f};

    void buildGeometry();
    void compileShaders();
};
