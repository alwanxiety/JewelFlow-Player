#include "Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>

struct Vertex {
    glm::vec3 pos;
    glm::vec3 norm;
    glm::vec2 uv;
};

static constexpr float kBoxW = 1.48f;
static constexpr float kBoxH = 1.34f;
static constexpr float kBoxD = 0.088f;
static constexpr float kSpacing = 0.21f;
static constexpr float kStartX = -1.20f;
static constexpr float kCos45 = 0.70710678f;

static const char* kBgVS = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.999, 1.0);
}
)";

static const char* kBgFS = R"(
#version 330 core
out vec4 FragColor;
in vec2 vUV;
uniform vec3 uAccent;
uniform float uTime;

void main() {
    vec3 topCol = vec3(0.068, 0.075, 0.098);
    vec3 botCol = vec3(0.032, 0.036, 0.048);
    vec3 col = mix(botCol, topCol, smoothstep(0.0, 1.0, vUV.y));

    vec2 glowCenter = vec2(0.50 + 0.05 * sin(uTime * 0.30), 0.50);
    float d = length((vUV - glowCenter) * vec2(1.5, 1.0));
    col += uAccent * exp(-d * d * 2.6) * 0.17;

    float horizon = 0.235;
    float shelfLine = smoothstep(0.004, 0.0, abs(vUV.y - horizon));
    col += (vec3(0.22, 0.25, 0.32) + uAccent * 0.25) * shelfLine * 0.42;

    if (vUV.y < horizon) {
        float t = (horizon - vUV.y) / horizon;
        vec3 reflCol = vec3(0.022, 0.025, 0.035) + uAccent * exp(-t * 3.5) * 0.05;
        col = mix(col, reflCol, smoothstep(0.0, 1.0, t) * 0.78);
    }

    FragColor = vec4(col, 1.0);
}
)";

static const char* kCaseVS = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vUV = aUV;
    gl_Position = uProj * uView * wp;
}
)";

static const char* kCaseFS = R"(
#version 330 core
out vec4 FragColor;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;

uniform vec3 uCamPos;
uniform vec3 uBaseColor;
uniform vec3 uAccent;
uniform int uType; // 0: Tray, 1: Spine (Procedural), 2: Cover, 3: Glass Lid, 4: Badge
uniform sampler2D uTex;
uniform float uOpacity;
uniform float uHighlight;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);

    vec3 L1 = normalize(vec3(0.42, 0.78, 0.88));
    vec3 L2 = normalize(vec3(-0.65, 0.30, 0.55));
    float diff1 = max(dot(N, L1), 0.0);
    float diff2 = max(dot(N, L2), 0.0);
    float lighting = 0.80 + 0.22 * diff1 + 0.08 * diff2;

    if (uType == 1) {
        vec3 baseSpine = vec3(0.11, 0.125, 0.16);

        float topCap = 1.0 - smoothstep(0.935, 0.942, vUV.y);
        float botCap = smoothstep(0.058, 0.065, vUV.y);
        float capMask = 1.0 - (topCap * botCap);

        float frontStripe = smoothstep(0.76, 0.80, vUV.x) * (1.0 - smoothstep(0.88, 0.92, vUV.x));

        vec3 col = mix(baseSpine, uAccent * 0.92, clamp(capMask + frontStripe * 0.65, 0.0, 1.0));

        float edgeDist = min(vUV.x, 1.0 - vUV.x);
        float aa = max(fwidth(edgeDist), 0.005);
        float edgeBevel = (1.0 - smoothstep(0.06, 0.06 + aa * 2.0, edgeDist)) * 0.18;

        vec3 H = normalize(L1 + V);
        float spec = pow(max(dot(N, H), 0.0), 42.0) * 0.24;

        vec3 finalCol = col * lighting + vec3(spec + edgeBevel) + uAccent * (uHighlight * 0.32);
        FragColor = vec4(finalCol, uOpacity);
    }
    else if (uType == 2) {
        vec3 col = texture(uTex, vUV).rgb;
        vec3 finalCol = col * (0.86 + 0.18 * diff1) + uAccent * (uHighlight * 0.18);
        FragColor = vec4(finalCol, uOpacity);
    }
    else if (uType == 4) {
        vec4 c = texture(uTex, vUV);
        FragColor = vec4(c.rgb, c.a * uOpacity);
    }
    else if (uType == 3) {
        vec3 H1 = normalize(L1 + V);
        vec3 H2 = normalize(L2 + V);
        float spec1 = pow(max(dot(N, H1), 0.0), 64.0);
        float spec2 = pow(max(dot(N, H2), 0.0), 24.0) * 0.25;
        float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(N, V), 0.0), 3.4);

        float dEdge = min(min(vUV.x, 1.0 - vUV.x), min(vUV.y, 1.0 - vUV.y));
        float aa = max(fwidth(dEdge), 0.002);
        float rim = (1.0 - smoothstep(0.008, 0.008 + aa * 2.0, dEdge)) * 0.48;

        float hingeDist = abs(vUV.x - 0.065);
        float hingeLine = (1.0 - smoothstep(0.002, 0.002 + fwidth(vUV.x) * 1.5, hingeDist)) * 0.26;
        float sheen = smoothstep(0.15, 0.85, vUV.x * 0.6 + vUV.y * 0.4) * 0.08;

        vec3 glassCol = vec3(0.94, 0.97, 1.0) * (spec1 * 1.25 + spec2 + fresnel * 0.32 + rim + hingeLine + sheen);
        glassCol += uAccent * (uHighlight * 0.45 * (rim + fresnel));

        float alpha = clamp(0.04 + fresnel * 0.26 + spec1 * 0.50 + rim * 0.55 + hingeLine * 0.45 + uHighlight * 0.14, 0.03, 0.65) * uOpacity;
        FragColor = vec4(glassCol, alpha);
    }
    else {
        vec3 H = normalize(L1 + V);
        float spec = pow(max(dot(N, H), 0.0), 28.0) * 0.20;
        vec3 trayCol = uBaseColor * lighting + vec3(spec) + uAccent * (uHighlight * 0.20);
        FragColor = vec4(trayCol, uOpacity);
    }
}
)";

static const char* kShadowFS = R"(
#version 330 core
out vec4 FragColor;
in vec2 vUV;

uniform vec3 uColor;
uniform float uLift;

void main() {
    vec2 p = vUV * 2.0 - 1.0;

    vec2 boxDist = abs(p) / vec2(0.68, 0.14);
    float contact = 1.0 - smoothstep(0.35, 1.05 + uLift * 0.6, max(boxDist.x, boxDist.y));
    contact = contact * contact * (0.62 * (1.0 - uLift * 0.55));

    float rad = length(p * vec2(0.85, 1.45));
    float soft = smoothstep(1.0, 0.0, rad);
    soft = soft * soft * (0.28 * (1.0 - uLift * 0.25));

    float alpha = clamp(contact + soft, 0.0, 0.72);
    FragColor = vec4(uColor, alpha);
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
    }
    return s;
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (m_bgShader) glDeleteProgram(m_bgShader);
    if (m_caseShader) glDeleteProgram(m_caseShader);
    if (m_shadowShader) glDeleteProgram(m_shadowShader);

    if (m_bgVAO) glDeleteVertexArrays(1, &m_bgVAO);
    if (m_bgVBO) glDeleteBuffers(1, &m_bgVBO);

    if (m_boxVAO) glDeleteVertexArrays(1, &m_boxVAO);
    if (m_boxVBO) glDeleteBuffers(1, &m_boxVBO);
    if (m_boxEBO) glDeleteBuffers(1, &m_boxEBO);

    if (m_shadowVAO) glDeleteVertexArrays(1, &m_shadowVAO);
    if (m_shadowVBO) glDeleteBuffers(1, &m_shadowVBO);

    if (m_badgeVAO) glDeleteVertexArrays(1, &m_badgeVAO);
    if (m_badgeVBO) glDeleteBuffers(1, &m_badgeVBO);
}

void Renderer::resize(int width, int height) {
    int w = (width > 0) ? width : 1280;
    int h = (height > 0) ? height : 720;
    if (w == m_width && h == m_height) return;

    m_width = w;
    m_height = h;
    glViewport(0, 0, m_width, m_height);

    m_proj = glm::perspective(glm::radians(28.0f), static_cast<float>(m_width) / static_cast<float>(m_height), 0.1f, 50.0f);
    m_camPos = glm::vec3(0.0f, 0.12f, 5.6f);
    m_view = glm::lookAt(m_camPos, glm::vec3(0.0f, -0.08f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

void Renderer::compileShaders() {
    GLuint bgVs = compile(GL_VERTEX_SHADER, kBgVS);
    GLuint bgFs = compile(GL_FRAGMENT_SHADER, kBgFS);
    m_bgShader = glCreateProgram();
    glAttachShader(m_bgShader, bgVs);
    glAttachShader(m_bgShader, bgFs);
    glLinkProgram(m_bgShader);
    glDeleteShader(bgVs);
    glDeleteShader(bgFs);

    m_bgLocAccent = glGetUniformLocation(m_bgShader, "uAccent");
    m_bgLocTime   = glGetUniformLocation(m_bgShader, "uTime");

    GLuint vs = compile(GL_VERTEX_SHADER, kCaseVS);
    GLuint fsCase = compile(GL_FRAGMENT_SHADER, kCaseFS);
    m_caseShader = glCreateProgram();
    glAttachShader(m_caseShader, vs);
    glAttachShader(m_caseShader, fsCase);
    glLinkProgram(m_caseShader);

    m_caseLocModel     = glGetUniformLocation(m_caseShader, "uModel");
    m_caseLocView      = glGetUniformLocation(m_caseShader, "uView");
    m_caseLocProj      = glGetUniformLocation(m_caseShader, "uProj");
    m_caseLocCamPos    = glGetUniformLocation(m_caseShader, "uCamPos");
    m_caseLocBaseColor = glGetUniformLocation(m_caseShader, "uBaseColor");
    m_caseLocAccent    = glGetUniformLocation(m_caseShader, "uAccent");
    m_caseLocType      = glGetUniformLocation(m_caseShader, "uType");
    m_caseLocTex       = glGetUniformLocation(m_caseShader, "uTex");
    m_caseLocOpacity   = glGetUniformLocation(m_caseShader, "uOpacity");
    m_caseLocHighlight = glGetUniformLocation(m_caseShader, "uHighlight");

    GLuint fsShadow = compile(GL_FRAGMENT_SHADER, kShadowFS);
    m_shadowShader = glCreateProgram();
    glAttachShader(m_shadowShader, vs);
    glAttachShader(m_shadowShader, fsShadow);
    glLinkProgram(m_shadowShader);

    m_shLocModel = glGetUniformLocation(m_shadowShader, "uModel");
    m_shLocView  = glGetUniformLocation(m_shadowShader, "uView");
    m_shLocProj  = glGetUniformLocation(m_shadowShader, "uProj");
    m_shLocColor = glGetUniformLocation(m_shadowShader, "uColor");
    m_shLocLift  = glGetUniformLocation(m_shadowShader, "uLift");

    glDeleteShader(vs);
    glDeleteShader(fsCase);
    glDeleteShader(fsShadow);
}

void Renderer::buildGeometry() {
    float bgQuad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };
    glGenVertexArrays(1, &m_bgVAO);
    glGenBuffers(1, &m_bgVBO);
    glBindVertexArray(m_bgVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_bgVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bgQuad), bgQuad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    const float w = kBoxW, h = kBoxH, d = kBoxD;

    std::vector<Vertex> bVerts = {
        // Face 0: Front Cover (+Z)
        { {0.0f, -h*0.5f,  d*0.5f + 0.002f}, {0,0, 1}, {0,0} },
        { {   w, -h*0.5f,  d*0.5f + 0.002f}, {0,0, 1}, {1,0} },
        { {   w,  h*0.5f,  d*0.5f + 0.002f}, {0,0, 1}, {1,1} },
        { {0.0f,  h*0.5f,  d*0.5f + 0.002f}, {0,0, 1}, {0,1} },
        // Face 1: Left Spine (-X)
        { {0.0f, -h*0.5f, -d*0.5f}, {-1,0,0}, {0,0} },
        { {0.0f, -h*0.5f,  d*0.5f}, {-1,0,0}, {1,0} },
        { {0.0f,  h*0.5f,  d*0.5f}, {-1,0,0}, {1,1} },
        { {0.0f,  h*0.5f, -d*0.5f}, {-1,0,0}, {0,1} },
        // Face 2: Back (-Z)
        { {   w, -h*0.5f, -d*0.5f}, {0,0,-1}, {0,0} },
        { {0.0f, -h*0.5f, -d*0.5f}, {0,0,-1}, {1,0} },
        { {0.0f,  h*0.5f, -d*0.5f}, {0,0,-1}, {1,1} },
        { {   w,  h*0.5f, -d*0.5f}, {0,0,-1}, {0,1} },
        // Face 3: Right (+X)
        { {   w, -h*0.5f,  d*0.5f}, { 1,0,0}, {0,0} },
        { {   w, -h*0.5f, -d*0.5f}, { 1,0,0}, {1,0} },
        { {   w,  h*0.5f, -d*0.5f}, { 1,0,0}, {1,1} },
        { {   w,  h*0.5f,  d*0.5f}, { 1,0,0}, {0,1} },
        // Face 4: Top (+Y)
        { {0.0f,  h*0.5f,  d*0.5f}, {0, 1,0}, {0,0} },
        { {   w,  h*0.5f,  d*0.5f}, {0, 1,0}, {1,0} },
        { {   w,  h*0.5f, -d*0.5f}, {0, 1,0}, {1,1} },
        { {0.0f,  h*0.5f, -d*0.5f}, {0, 1,0}, {0,1} },
        // Face 5: Bottom (-Y)
        { {0.0f, -h*0.5f, -d*0.5f}, {0,-1,0}, {0,0} },
        { {   w, -h*0.5f, -d*0.5f}, {0,-1,0}, {1,0} },
        { {   w, -h*0.5f,  d*0.5f}, {0,-1,0}, {1,1} },
        { {0.0f, -h*0.5f,  d*0.5f}, {0,-1,0}, {0,1} }
    };

    std::vector<uint32_t> bInds;
    for (uint32_t i = 0; i < 6; ++i) {
        uint32_t off = i * 4;
        bInds.insert(bInds.end(), {off, off + 1, off + 2, off, off + 2, off + 3});
    }

    glGenVertexArrays(1, &m_boxVAO);
    glGenBuffers(1, &m_boxVBO);
    glGenBuffers(1, &m_boxEBO);
    glBindVertexArray(m_boxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_boxVBO);
    glBufferData(GL_ARRAY_BUFFER, bVerts.size() * sizeof(Vertex), bVerts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_boxEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, bInds.size() * sizeof(uint32_t), bInds.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)12);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)24);
    glEnableVertexAttribArray(2);

    float badgeH = h * 0.33f;
    std::vector<Vertex> badgeVerts = {
        { {0.0f, -h*0.5f,          d*0.5f + 0.005f}, {0,0,1}, {0,0} },
        { {   w, -h*0.5f,          d*0.5f + 0.005f}, {0,0,1}, {1,0} },
        { {   w, -h*0.5f + badgeH, d*0.5f + 0.005f}, {0,0,1}, {1,1} },
        { {0.0f, -h*0.5f + badgeH, d*0.5f + 0.005f}, {0,0,1}, {0,1} }
    };
    glGenVertexArrays(1, &m_badgeVAO);
    glGenBuffers(1, &m_badgeVBO);
    glBindVertexArray(m_badgeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_badgeVBO);
    glBufferData(GL_ARRAY_BUFFER, badgeVerts.size() * sizeof(Vertex), badgeVerts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)12);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)24);
    glEnableVertexAttribArray(2);

    std::vector<Vertex> sVerts = {
        { {-1.0f, 0.0f, -1.0f}, {0,1,0}, {0,0} },
        { { 1.0f, 0.0f, -1.0f}, {0,1,0}, {1,0} },
        { { 1.0f, 0.0f,  1.0f}, {0,1,0}, {1,1} },
        { {-1.0f, 0.0f,  1.0f}, {0,1,0}, {0,1} }
    };
    glGenVertexArrays(1, &m_shadowVAO);
    glGenBuffers(1, &m_shadowVBO);
    glBindVertexArray(m_shadowVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_shadowVBO);
    glBufferData(GL_ARRAY_BUFFER, sVerts.size() * sizeof(Vertex), sVerts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)12);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)24);
    glEnableVertexAttribArray(2);
}

void Renderer::init() {
    compileShaders();
    buildGeometry();
    resize(1360, 780);
}

glm::vec2 Renderer::getBoxBottomScreenPos(int slotIndex, float hoverWeight, float shelfScroll) const {
    glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    float xPos = kStartX + (static_cast<float>(slotIndex) * kSpacing) - shelfScroll;
    float curY = hoverWeight * 0.22f;

    // Аналитическая точка (0.32, -0.72, 0.04), повернутая на +45 градусов вокруг Y
    glm::vec3 worldPt(
        xPos + (0.32f * kCos45 + 0.04f * kCos45),
        curY - 0.72f,
        -0.32f * kCos45 + 0.04f * kCos45
    );
    glm::vec3 screenPt = glm::project(worldPt, m_view, m_proj, viewport);
    return glm::vec2(screenPt.x, static_cast<float>(m_height) - screenPt.y);
}

glm::vec2 Renderer::getBoxTopScreenPos(int slotIndex, float hoverWeight, float shelfScroll) const {
    glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    float xPos = kStartX + (static_cast<float>(slotIndex) * kSpacing) - shelfScroll;
    float curY = hoverWeight * 0.22f;

    glm::vec3 worldPt(
        xPos + 0.16f * kCos45,
        curY + 0.72f,
        -0.16f * kCos45
    );
    glm::vec3 screenPt = glm::project(worldPt, m_view, m_proj, viewport);
    return glm::vec2(screenPt.x, static_cast<float>(m_height) - screenPt.y);
}

// Сверхбыстрый 3D Raycast без glm::inverse(mat4) в цикле — аналитический поворот луча на -45 градусов вокруг Y
int Renderer::pickTrack(double mouseX,
                        double mouseY,
                        const std::vector<int>& visibleIndices,
                        const std::vector<float>& hoverWeights,
                        float shelfScroll) const {
    if (visibleIndices.empty() || m_width <= 0 || m_height <= 0) return -1;

    glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    float winX = static_cast<float>(mouseX);
    float winY = static_cast<float>(m_height) - static_cast<float>(mouseY);

    glm::vec3 rayNear = glm::unProject(glm::vec3(winX, winY, 0.0f), m_view, m_proj, viewport);
    glm::vec3 rayFar  = glm::unProject(glm::vec3(winX, winY, 1.0f), m_view, m_proj, viewport);
    glm::vec3 rayDir  = glm::normalize(rayFar - rayNear);

    // Направление луча в локальной системе координат бокса одинаково для всех боксов на полке!
    const float localDirX = (rayDir.x - rayDir.z) * kCos45;
    const float localDirY = rayDir.y;
    const float localDirZ = (rayDir.x + rayDir.z) * kCos45;
    const float localDir[3] = {localDirX, localDirY, localDirZ};

    const float bMin[3] = {-0.02f, -kBoxH * 0.5f, -kBoxD * 0.65f};
    const float bMax[3] = {kBoxW * 0.85f, kBoxH * 0.5f, kBoxD * 0.65f};

    int bestTrack = -1;
    float bestT = 1e9f;

    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        float xPos = kStartX + (static_cast<float>(slot) * kSpacing) - shelfScroll;
        if (xPos < -2.9f || xPos > 2.6f) continue;

        int realIdx = visibleIndices[slot];
        float hW = (realIdx < static_cast<int>(hoverWeights.size())) ? hoverWeights[realIdx] : 0.0f;
        float curY = hW * 0.22f;

        float dx = rayNear.x - xPos;
        float dy = rayNear.y - curY;
        float dz = rayNear.z;
        const float localOrig[3] = {
            (dx - dz) * kCos45,
            dy,
            (dx + dz) * kCos45
        };

        float tMin = -1e9f, tMax = 1e9f;
        bool hit = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(localDir[axis]) < 1e-6f) {
                if (localOrig[axis] < bMin[axis] || localOrig[axis] > bMax[axis]) {
                    hit = false;
                    break;
                }
            } else {
                float invD = 1.0f / localDir[axis];
                float t1 = (bMin[axis] - localOrig[axis]) * invD;
                float t2 = (bMax[axis] - localOrig[axis]) * invD;
                if (t1 > t2) std::swap(t1, t2);
                if (t1 > tMin) tMin = t1;
                if (t2 < tMax) tMax = t2;
                if (tMin > tMax) {
                    hit = false;
                    break;
                }
            }
        }

        if (hit && tMax > 0.0f && tMin < bestT) {
            bestT = tMin;
            bestTrack = realIdx;
        }
    }

    return bestTrack;
}

void Renderer::renderShelf(const std::vector<TrackData>& playlist,
                           const std::vector<int>& visibleIndices,
                           const std::vector<float>& hoverWeights,
                           float shelfScroll,
                           int selectedIndex,
                           int activePlayingIndex,
                           int rouletteHighlightIndex,
                           float discFlightProgress,
                           float pulseTime,
                           const glm::vec3& themeAccent) {
    glClearColor(0.04f, 0.045f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_MULTISAMPLE);

    // 0. Отрисовка студийного фона
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(m_bgShader);
    glUniform3fv(m_bgLocAccent, 1, glm::value_ptr(themeAccent));
    glUniform1f(m_bgLocTime, pulseTime);
    glBindVertexArray(m_bgVAO);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    if (visibleIndices.empty()) return;

    // 1. Двухслойные тени под видимыми кейсами
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUseProgram(m_shadowShader);
    glUniformMatrix4fv(m_shLocProj, 1, GL_FALSE, glm::value_ptr(m_proj));
    glUniformMatrix4fv(m_shLocView, 1, GL_FALSE, glm::value_ptr(m_view));
    glBindVertexArray(m_shadowVAO);

    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        float xPos = kStartX + (static_cast<float>(slot) * kSpacing) - shelfScroll;
        if (xPos < -2.9f || xPos > 2.6f) continue;

        int realIdx = visibleIndices[slot];
        float hW = (realIdx < static_cast<int>(hoverWeights.size())) ? hoverWeights[realIdx] : 0.0f;

        bool isGlow = (realIdx == activePlayingIndex || realIdx == rouletteHighlightIndex);
        glm::vec3 shadowCol = isGlow
            ? (playlist[realIdx].accentColor * 0.42f)
            : glm::vec3(0.008f, 0.010f, 0.016f);
        glUniform3fv(m_shLocColor, 1, glm::value_ptr(shadowCol));
        glUniform1f(m_shLocLift, hW);

        glm::mat4 sm = glm::translate(glm::mat4(1.0f), glm::vec3(xPos + 0.50f, -0.68f, -0.46f));
        sm = glm::rotate(sm, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        sm = glm::scale(sm, glm::vec3(0.95f, 1.0f, 0.36f));
        glUniformMatrix4fv(m_shLocModel, 1, GL_FALSE, glm::value_ptr(sm));
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    glUseProgram(m_caseShader);
    glUniformMatrix4fv(m_caseLocProj, 1, GL_FALSE, glm::value_ptr(m_proj));
    glUniformMatrix4fv(m_caseLocView, 1, GL_FALSE, glm::value_ptr(m_view));
    glUniform3fv(m_caseLocCamPos, 1, glm::value_ptr(m_camPos));
    glUniform1i(m_caseLocTex, 0);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    auto drawClosedCase = [&](int trackIdx, const glm::mat4& modelMat, float opacity, float highlight) {
        glUniform1f(m_caseLocOpacity, opacity);
        glUniform1f(m_caseLocHighlight, highlight);
        glUniform3fv(m_caseLocAccent, 1, glm::value_ptr(playlist[trackIdx].accentColor));
        glUniformMatrix4fv(m_caseLocModel, 1, GL_FALSE, glm::value_ptr(modelMat));

        if (opacity < 0.999f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        glBindVertexArray(m_boxVAO);

        // 1) Боковая грань (процедурный минималистичный корешок без текста)
        glUniform1i(m_caseLocType, 1);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(6 * sizeof(uint32_t)));

        // 2) Задний лоток
        glUniform1i(m_caseLocType, 0);
        glUniform3f(m_caseLocBaseColor, 0.15f, 0.17f, 0.22f);
        glDrawElements(GL_TRIANGLES, 24, GL_UNSIGNED_INT, (void*)(12 * sizeof(uint32_t)));

        // 3) Лицевая обложка
        glUniform1i(m_caseLocType, 2);
        glBindTexture(GL_TEXTURE_2D, playlist[trackIdx].coverTex);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(0 * sizeof(uint32_t)));

        // 4) Плашка с названием и исполнителем внизу обложки
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1i(m_caseLocType, 4);
        glBindTexture(GL_TEXTURE_2D, playlist[trackIdx].badgeTex);
        glBindVertexArray(m_badgeVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        // 5) Акриловый глянец поверх закрытого бокса
        glDepthMask(GL_FALSE);
        glUniform1i(m_caseLocType, 3);
        glm::mat4 glassM = glm::translate(modelMat, glm::vec3(0.0f, 0.0f, 0.007f));
        glUniformMatrix4fv(m_caseLocModel, 1, GL_FALSE, glm::value_ptr(glassM));
        glBindVertexArray(m_boxVAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(0 * sizeof(uint32_t)));
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    };

    int flyingSlot = -1;

    // 2. Рендер видимых CD-боксов на полке
    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        int realIdx = visibleIndices[slot];
        if (realIdx == selectedIndex) {
            flyingSlot = static_cast<int>(slot);
        }

        float xPos = kStartX + (static_cast<float>(slot) * kSpacing) - shelfScroll;
        if (xPos < -2.9f || xPos > 2.6f) continue;

        float hW = (realIdx < static_cast<int>(hoverWeights.size())) ? hoverWeights[realIdx] : 0.0f;
        float curY = hW * 0.22f;
        if (realIdx == activePlayingIndex) {
            curY += std::abs(std::sin(pulseTime * 3.2f)) * 0.022f;
        }

        float highlight = (realIdx == rouletteHighlightIndex) ? 1.0f : (hW * 0.45f);

        glm::mat4 baseM(1.0f);
        baseM = glm::translate(baseM, glm::vec3(xPos, curY, 0.0f));
        baseM = glm::rotate(baseM, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        drawClosedCase(realIdx, baseM, 1.0f, highlight);
    }

    // 3. Анимация плавного перетекания выбранного бокса в нижний бар плеера
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(playlist.size()) &&
        discFlightProgress > 0.001f && discFlightProgress < 0.995f) {
        float t = discFlightProgress;
        float ease = (t < 0.5f) ? (4.0f * t * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f);

        float startXPos = (flyingSlot >= 0)
            ? (kStartX + static_cast<float>(flyingSlot) * kSpacing - shelfScroll)
            : 0.0f;
        float hW = (selectedIndex < static_cast<int>(hoverWeights.size())) ? hoverWeights[selectedIndex] : 0.0f;

        glm::vec3 startPos(startXPos, hW * 0.22f, 0.0f);
        glm::vec3 targetPos(-0.78f, -0.92f, 2.35f);

        glm::vec3 curPos = glm::mix(startPos, targetPos, ease);
        curPos.y += std::sin(ease * 3.14159265f) * 0.22f;
        curPos.z += std::sin(ease * 3.14159265f) * 0.45f;

        float curYaw = glm::mix(45.0f, 0.0f, ease);
        float curScale = glm::mix(1.0f, 0.16f, ease);
        float alpha = std::clamp(1.0f - (ease - 0.68f) * 3.125f, 0.0f, 1.0f);

        glm::mat4 flyM(1.0f);
        flyM = glm::translate(flyM, curPos);
        flyM = glm::rotate(flyM, glm::radians(curYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        flyM = glm::scale(flyM, glm::vec3(curScale));

        drawClosedCase(selectedIndex, flyM, alpha, 0.6f);
    }

    glDisable(GL_CULL_FACE);
}