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

static constexpr glm::vec3 kInspectCenter{0.0f, 0.07f, 1.05f};
static constexpr float kInspectTargetScale = 0.95f;

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

    // Оптимизированное радиальное свечение: замена length() и exp() на квадратичный спад (dot + clamp)
    vec2 glowCenter = vec2(0.50 + 0.05 * sin(uTime * 0.30), 0.50);
    vec2 dVec = (vUV - glowCenter) * vec2(1.5, 1.0);
    float d2 = dot(dVec, dVec);
    float glow = clamp(1.0 - d2 * 1.6, 0.0, 1.0);
    col += uAccent * (glow * glow * 0.17);

    float horizon = 0.235;
    float shelfLine = smoothstep(0.004, 0.0, abs(vUV.y - horizon));
    col += (vec3(0.22, 0.25, 0.32) + uAccent * 0.25) * shelfLine * 0.42;

    if (vUV.y < horizon) {
        float t = (horizon - vUV.y) / horizon;
        float tFade = clamp(1.0 - t * 1.5, 0.0, 1.0);
        vec3 reflCol = vec3(0.022, 0.025, 0.035) + uAccent * (tFade * tFade * 0.05);
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

// uType:
// 0: Tray edges
// 1: Procedural Spine
// 2: Front Cover
// 3: Acrylic Glass Lid
// 4: Front Bottom Badge
// 5: Back Cover Inlay (Задняя сторона бокса для отображения текста песни и караоке!)
static const char* kCaseFS = R"(
#version 330 core
out vec4 FragColor;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;

uniform vec3 uCamPos;
uniform vec3 uBaseColor;
uniform vec3 uAccent;
uniform int uType;
uniform sampler2D uTex;
uniform float uOpacity;
uniform float uHighlight;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);

    const vec3 L1 = vec3(0.336355, 0.624660, 0.704745);
    const vec3 L2 = vec3(-0.720003, 0.332309, 0.609234);
    float diff1 = max(dot(N, L1), 0.0);
    float diff2 = max(dot(N, L2), 0.0);
    float lighting = 0.80 + 0.22 * diff1 + 0.08 * diff2;

    if (uType == 1) {
        vec3 baseSpine = vec3(0.11, 0.125, 0.16);

        float topCap = 1.0 - smoothstep(0.935, 0.942, vUV.y);
        float botCap = smoothstep(0.058, 0.065, vUV.y);
        float capMask = 1.0 - (topCap * botCap);

        vec3 col = mix(baseSpine, uAccent * 0.92, clamp(capMask * 0.75, 0.0, 1.0));

        // Мягкое скругление только заднего ребра корешка (стык с задней крышкой).
        // На переднем стыке с обложкой (vUV.x = 1.0) искусственная полоса убрана, чтобы исключить оптическую щель
        float backEdgeDist = vUV.x;
        float aa = max(fwidth(backEdgeDist), 0.003);
        float edgeBevel = (1.0 - smoothstep(0.025, 0.025 + aa * 2.5, backEdgeDist)) * 0.15;

        vec3 H = normalize(L1 + V);
        float spec = pow(max(dot(N, H), 0.0), 42.0) * 0.24;

        vec3 finalCol = col * lighting + vec3(spec + edgeBevel) + uAccent * (uHighlight * 0.32);
        FragColor = vec4(finalCol, uOpacity);
    }
    else if (uType == 2) {
        vec3 col = texture(uTex, vUV).rgb;
        // Кристальная глубина цвета обложки с сочным освещением
        vec3 finalCol = col * (0.92 + 0.16 * diff1) + uAccent * (uHighlight * 0.18);
        FragColor = vec4(finalCol, uOpacity);
    }
    else if (uType == 4) {
        vec4 c = texture(uTex, vUV);
        FragColor = vec4(c.rgb, c.a * uOpacity);
    }
    else if (uType == 5) {
        // Задняя сторона CD-бокса (Back Inlay Card — аутентичный кристально чёткий полиграфический арт):
        // Прямое сэмплирование полноразмерной текстуры обложки без размытия для максимального качества
        vec2 coverUV = vec2(vUV.x, vUV.y);
        vec3 coverArt = texture(uTex, coverUV).rgb;

        // Сочный полиграфический задник с глубоким студийным основанием (высокое присутствие оригинального арта)
        vec3 darkStudio = vec3(0.018, 0.022, 0.030);
        vec3 inlayCol = mix(darkStudio, coverArt, 0.82);

        // Мягкий контраст кривой (S-curve) для кинематографичной насыщенности
        inlayCol = mix(inlayCol, inlayCol * inlayCol * (3.0 - 2.0 * inlayCol), 0.22);

        // Тактильное микрозерно художественной бумаги
        float grain = fract(sin(dot(vUV * 420.0, vec2(12.9898, 78.233))) * 43758.5453);
        inlayCol += (grain - 0.5) * 0.007;

        // Деликатная радиальная виньетка: сохраняет читаемость караоке-текста, оставляя арт ярким и узнаваемым
        float centerDist = length((vUV - vec2(0.5, 0.50)) * vec2(1.15, 1.0));
        float centerDarken = 1.0 - smoothstep(0.15, 0.90, 1.0 - centerDist) * 0.12;
        float cornerVignette = 1.0 - smoothstep(0.48, 0.98, centerDist) * 0.22;
        inlayCol *= centerDarken * cornerVignette;

        // Мягкое атмосферное свечение акцента из глубины
        inlayCol += uAccent * exp(-centerDist * centerDist * 2.8) * 0.14;

        // Тончайшая акцентная внутренняя рамка по периметру полиграфического вкладыша
        float dEdge = min(min(vUV.x, 1.0 - vUV.x), min(vUV.y, 1.0 - vUV.y));
        float innerFrame = smoothstep(0.012, 0.015, dEdge) * (1.0 - smoothstep(0.016, 0.019, dEdge));
        inlayCol = mix(inlayCol, uAccent * 0.70, innerFrame * 0.40);

        // Акриловый глянец поверхности кейса и деликатные диагональные блики света
        vec3 H = normalize(L1 + V);
        float spec = pow(max(dot(N, H), 0.0), 48.0) * 0.14;
        float diagSheen1 = smoothstep(0.15, 0.85, vUV.x * 0.55 + vUV.y * 0.45) * 0.040;
        float diagSheen2 = smoothstep(0.35, 0.65, vUV.x * 0.40 + vUV.y * 0.60) * 0.020;
        float rim = (1.0 - smoothstep(0.002, 0.008, dEdge)) * 0.16;

        vec3 finalCol = inlayCol * (0.92 + 0.12 * diff1) + vec3(spec + diagSheen1 + diagSheen2) + (vec3(0.65, 0.72, 0.85) + uAccent * 0.25) * rim;
        FragColor = vec4(finalCol, uOpacity);
    }
    else if (uType == 3) {
        vec3 H1 = normalize(L1 + V);
        vec3 H2 = normalize(L2 + V);
        float spec1 = pow(max(dot(N, H1), 0.0), 64.0);
        float spec2 = pow(max(dot(N, H2), 0.0), 24.0) * 0.25;
        float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(N, V), 0.0), 3.4);

        // Ободок акрила: мягко затухает на левом ребре (vUV.x = 0), чтобы не образовывать двойную белую полосу с корешком
        float dEdge = min(min(vUV.x * 4.0, 1.0 - vUV.x), min(vUV.y, 1.0 - vUV.y));
        float aa = max(fwidth(dEdge), 0.002);
        float rim = (1.0 - smoothstep(0.008, 0.008 + aa * 2.0, dEdge)) * 0.40;

        float hingeDist = abs(vUV.x - 0.065);
        float hingeLine = (1.0 - smoothstep(0.002, 0.002 + fwidth(vUV.x) * 1.5, hingeDist)) * 0.14;
        float sheen = smoothstep(0.15, 0.85, vUV.x * 0.6 + vUV.y * 0.4) * 0.03;

        vec3 glassCol = vec3(0.94, 0.97, 1.0) * (spec1 * 1.25 + spec2 + fresnel * 0.32 + rim + hingeLine + sheen);
        glassCol += uAccent * (uHighlight * 0.45 * (rim + fresnel));

        // Прозрачный акрил без белесой плёнки поверх обложки (минимальный базовый альфа)
        float alpha = clamp(0.005 + fresnel * 0.22 + spec1 * 0.55 + rim * 0.55 + hingeLine * 0.30 + uHighlight * 0.14, 0.0, 0.65) * uOpacity;
        FragColor = vec4(glassCol, alpha);
    }
    else if (uType == 6) {
        // Деликатное матовое 3D-отражение бокса (лицевая обложка + боковой корешок) в лакированном полу:
        // vUV.x: по ширине соответствующей грани
        // vUV.y: 0.0 у нижнего стыка с боксом -> 1.0 в глубине пола
        float fade = 1.0 - smoothstep(0.0, 0.95, vUV.y);
        fade = fade * fade;

        // Ослабление отражения при подъёме бокса (эффект отрыва от поверхности)
        float liftFade = clamp(1.0 - uHighlight * 1.6, 0.0, 1.0);

        vec3 reflCol;
        if (vNormal.y > 0.5) {
            // Отражение боковой грани (корешка) — матовый поликарбонат с акцентной нижней плашкой
            float capZone = 1.0 - smoothstep(0.03, 0.14, vUV.y);
            vec3 baseSpine = vec3(0.09, 0.105, 0.14);
            reflCol = mix(baseSpine, uAccent * 0.88, capZone * 0.75) + uAccent * 0.08;
        } else {
            // Отражение лицевой обложки:
            // Аппаратный textureLod (LOD 1.2 -> 2.8) для благородного матового рассеивания без резких пикселей
            float lod = mix(1.2, 2.8, vUV.y);
            vec2 reflUV = vec2(vUV.x, vUV.y * 0.52);
            vec3 col = textureLod(uTex, reflUV, lod).rgb;

            // Матовое рассеивание с благородной глубиной студийного лака
            vec3 matteDark = vec3(0.022, 0.026, 0.036);
            reflCol = mix(col * 0.70, matteDark, 0.16) + uAccent * 0.10;
        }

        float alpha = fade * (0.24 * uOpacity * liftFade);
        FragColor = vec4(reflCol, alpha);
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

    // Оптимизация: умножение на предрассчитанный вектор обратных величин вместо деления в пиксельном конвейере
    vec2 boxDist = abs(p) * vec2(1.470588, 7.142857);
    float contact = 1.0 - smoothstep(0.35, 1.05 + uLift * 0.6, max(boxDist.x, boxDist.y));
    contact = contact * contact * (0.62 * (1.0 - uLift * 0.55));

    float rad = length(p * vec2(0.85, 1.45));
    float soft = smoothstep(1.0, 0.0, rad);
    soft = soft * soft * (0.28 * (1.0 - uLift * 0.25));

    float alpha = clamp(contact + soft, 0.0, 0.72);
    FragColor = vec4(uColor, alpha);
}
)";

static const char* kFxaaVS = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kFxaaFS = R"(
#version 330 core
out vec4 FragColor;
in vec2 vUV;

uniform sampler2D uTex;
uniform vec2 uRcpFrame;

// Высокопроизводительный шейдер FXAA 3.11 Quality Preset
#define FXAA_REDUCE_MIN   (1.0 / 128.0)
#define FXAA_REDUCE_MUL   (1.0 / 8.0)
#define FXAA_SPAN_MAX     3.0

float rgb2luma(vec3 rgb) {
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 rgbM = texture(uTex, vUV).rgb;
    float lumaM = rgb2luma(rgbM);

    float lumaNW = rgb2luma(textureOffset(uTex, vUV, ivec2(-1, -1)).rgb);
    float lumaNE = rgb2luma(textureOffset(uTex, vUV, ivec2( 1, -1)).rgb);
    float lumaSW = rgb2luma(textureOffset(uTex, vUV, ivec2(-1,  1)).rgb);
    float lumaSE = rgb2luma(textureOffset(uTex, vUV, ivec2( 1,  1)).rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float lumaRange = lumaMax - lumaMin;
    // Порог контраста по стандарту NVIDIA FXAA: фильтруются только внешние силуэты 3D-боксов.
    // Внутренний арт обложек и текст остаются 100% чёткими и не замыливаются в оконном режиме!
    if (lumaRange < max(0.0833, lumaMax * 0.166)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * uRcpFrame;

    vec3 rgbA = 0.5 * (
        texture(uTex, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(uTex, vUV + dir * (2.0 / 3.0 - 0.5)).rgb
    );
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(uTex, vUV + dir * (0.0 / 3.0 - 0.5)).rgb +
        texture(uTex, vUV + dir * (3.0 / 3.0 - 0.5)).rgb
    );

    float lumaB = rgb2luma(rgbB);
    if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
        FragColor = vec4(rgbA, 1.0);
    } else {
        FragColor = vec4(rgbB, 1.0);
    }
}
)";

static const char* kCopyFS = R"(
#version 330 core
out vec4 FragColor;
in vec2 vUV;
uniform sampler2D uTex;
void main() {
    FragColor = texture(uTex, vUV);
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
    if (m_fxaaShader) glDeleteProgram(m_fxaaShader);
    if (m_copyShader) glDeleteProgram(m_copyShader);

    if (m_bgVAO) glDeleteVertexArrays(1, &m_bgVAO);
    if (m_bgVBO) glDeleteBuffers(1, &m_bgVBO);

    if (m_boxVAO) glDeleteVertexArrays(1, &m_boxVAO);
    if (m_boxVBO) glDeleteBuffers(1, &m_boxVBO);
    if (m_boxEBO) glDeleteBuffers(1, &m_boxEBO);

    if (m_shadowVAO) glDeleteVertexArrays(1, &m_shadowVAO);
    if (m_shadowVBO) glDeleteBuffers(1, &m_shadowVBO);

    if (m_badgeVAO) glDeleteVertexArrays(1, &m_badgeVAO);
    if (m_badgeVBO) glDeleteBuffers(1, &m_badgeVBO);

    if (m_reflVAO) glDeleteVertexArrays(1, &m_reflVAO);
    if (m_reflVBO) glDeleteBuffers(1, &m_reflVBO);
    if (m_reflEBO) glDeleteBuffers(1, &m_reflEBO);

    if (m_sceneFBO) {
        glDeleteFramebuffers(1, &m_sceneFBO);
        glDeleteTextures(1, &m_sceneColorTex);
        glDeleteRenderbuffers(1, &m_sceneDepthRB);
    }

    if (m_msaaFBO) {
        glDeleteFramebuffers(1, &m_msaaFBO);
        glDeleteRenderbuffers(1, &m_msaaColorRB);
        glDeleteRenderbuffers(1, &m_msaaDepthRB);
    }
}

void Renderer::resize(int width, int height) {
    int w = (width > 0) ? width : 1280;
    int h = (height > 0) ? height : 720;
    if (w == m_width && h == m_height && m_sceneFBO != 0 && m_msaaFBO != 0) return;

    m_width = w;
    m_height = h;
    glViewport(0, 0, m_width, m_height);

    m_proj = glm::perspective(glm::radians(28.0f), static_cast<float>(m_width) / static_cast<float>(m_height), 0.1f, 50.0f);
    m_camPos = glm::vec3(0.0f, 0.12f, 5.6f);
    m_view = glm::lookAt(m_camPos, glm::vec3(0.0f, -0.08f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // 1. Односэмпловый Scene FBO (для FXAA и режима без сглаживания — ультра-низкая нагрузка GPU)
    if (m_sceneFBO) {
        glDeleteFramebuffers(1, &m_sceneFBO);
        glDeleteTextures(1, &m_sceneColorTex);
        glDeleteRenderbuffers(1, &m_sceneDepthRB);
        m_sceneFBO = 0;
        m_sceneColorTex = 0;
        m_sceneDepthRB = 0;
    }

    glGenFramebuffers(1, &m_sceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);

    glGenTextures(1, &m_sceneColorTex);
    glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneColorTex, 0);

    glGenRenderbuffers(1, &m_sceneDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, m_sceneDepthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_width, m_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_sceneDepthRB);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 2. Аппаратный 2x MSAA Framebuffer (для режима классического MSAA)
    if (m_msaaFBO) {
        glDeleteFramebuffers(1, &m_msaaFBO);
        glDeleteRenderbuffers(1, &m_msaaColorRB);
        glDeleteRenderbuffers(1, &m_msaaDepthRB);
        m_msaaFBO = 0;
        m_msaaColorRB = 0;
        m_msaaDepthRB = 0;
    }

    GLint maxSamples = 2;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    m_samples = (maxSamples >= 2) ? 2 : 1;

    if (m_samples > 1) {
        glGenFramebuffers(1, &m_msaaFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);

        glGenRenderbuffers(1, &m_msaaColorRB);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColorRB);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msaaColorRB);

        glGenRenderbuffers(1, &m_msaaDepthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepthRB);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_DEPTH24_STENCIL8, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_msaaDepthRB);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &m_msaaFBO);
            glDeleteRenderbuffers(1, &m_msaaColorRB);
            glDeleteRenderbuffers(1, &m_msaaDepthRB);
            m_msaaFBO = 0;
            m_msaaColorRB = 0;
            m_msaaDepthRB = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
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

    // Компиляция пост-процессинг шейдера FXAA и копирования
    GLuint fxaaVs = compile(GL_VERTEX_SHADER, kFxaaVS);
    GLuint fxaaFs = compile(GL_FRAGMENT_SHADER, kFxaaFS);
    m_fxaaShader = glCreateProgram();
    glAttachShader(m_fxaaShader, fxaaVs);
    glAttachShader(m_fxaaShader, fxaaFs);
    glLinkProgram(m_fxaaShader);
    glDeleteShader(fxaaFs);

    m_fxaaLocTex      = glGetUniformLocation(m_fxaaShader, "uTex");
    m_fxaaLocRcpFrame = glGetUniformLocation(m_fxaaShader, "uRcpFrame");

    GLuint copyFs = compile(GL_FRAGMENT_SHADER, kCopyFS);
    m_copyShader = glCreateProgram();
    glAttachShader(m_copyShader, fxaaVs);
    glAttachShader(m_copyShader, copyFs);
    glLinkProgram(m_copyShader);
    glDeleteShader(fxaaVs);
    glDeleteShader(copyFs);

    m_copyLocTex = glGetUniformLocation(m_copyShader, "uTex");
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
        // Face 0: Front Cover (+Z) — плотно смыкается с левым корешком без зазора
        { {-0.0006f, -h*0.5f,  d*0.5f}, {0,0, 1}, {0,0} },
        { {       w, -h*0.5f,  d*0.5f}, {0,0, 1}, {1,0} },
        { {       w,  h*0.5f,  d*0.5f}, {0,0, 1}, {1,1} },
        { {-0.0006f,  h*0.5f,  d*0.5f}, {0,0, 1}, {0,1} },
        // Face 1: Left Spine (-X) — бесшовно смыкается с передней обложкой
        { {0.0f, -h*0.5f, -d*0.5f}, {-1,0,0}, {0,0} },
        { {0.0f, -h*0.5f,  d*0.5f + 0.0006f}, {-1,0,0}, {1,0} },
        { {0.0f,  h*0.5f,  d*0.5f + 0.0006f}, {-1,0,0}, {1,1} },
        { {0.0f,  h*0.5f, -d*0.5f}, {-1,0,0}, {0,1} },
        // Face 2: Back Inlay (-Z) — задняя крышка для отображения текста песни!
        { {   w, -h*0.5f, -d*0.5f}, {0,0,-1}, {0,0} },
        { {0.0f, -h*0.5f, -d*0.5f}, {0,0,-1}, {1,0} },
        { {0.0f,  h*0.5f, -d*0.5f}, {0,0,-1}, {1,1} },
        { {   w,  h*0.5f, -d*0.5f}, {0,0,-1}, {0,1} },
        // Face 3: Right (+X)
        { {   w, -h*0.5f,  d*0.5f + 0.0006f}, { 1,0,0}, {0,0} },
        { {   w, -h*0.5f, -d*0.5f}, { 1,0,0}, {1,0} },
        { {   w,  h*0.5f, -d*0.5f}, { 1,0,0}, {1,1} },
        { {   w,  h*0.5f,  d*0.5f + 0.0006f}, { 1,0,0}, {0,1} },
        // Face 4: Top (+Y)
        { {-0.0006f,  h*0.5f,  d*0.5f + 0.0006f}, {0, 1,0}, {0,0} },
        { {       w,  h*0.5f,  d*0.5f + 0.0006f}, {0, 1,0}, {1,0} },
        { {       w,  h*0.5f, -d*0.5f}, {0, 1,0}, {1,1} },
        { {-0.0006f,  h*0.5f, -d*0.5f}, {0, 1,0}, {0,1} },
        // Face 5: Bottom (-Y)
        { {-0.0006f, -h*0.5f, -d*0.5f}, {0,-1,0}, {0,0} },
        { {       w, -h*0.5f, -d*0.5f}, {0,-1,0}, {1,0} },
        { {       w, -h*0.5f,  d*0.5f + 0.0006f}, {0,-1,0}, {1,1} },
        { {-0.0006f, -h*0.5f,  d*0.5f + 0.0006f}, {0,-1,0}, {0,1} }
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
        { {-0.0006f, -h*0.5f,          d*0.5f + 0.0002f}, {0,0,1}, {0,0} },
        { {       w, -h*0.5f,          d*0.5f + 0.0002f}, {0,0,1}, {1,0} },
        { {       w, -h*0.5f + badgeH, d*0.5f + 0.0002f}, {0,0,1}, {1,1} },
        { {-0.0006f, -h*0.5f + badgeH, d*0.5f + 0.0002f}, {0,0,1}, {0,1} }
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

    // Геометрия нежного 3D-отражения (лицевая обложка + боковой корешок) в лакированном полу под CD-боксом
    float reflH = h * 0.42f;
    std::vector<Vertex> reflVerts = {
        // Quad 0: Отражение лицевой обложки (+Z, aNormal = {0, 0, 1} -> vNormal.y == 0.0)
        { {-0.0006f, -h*0.5f - reflH, d*0.5f}, {0,0,1}, {0.0f, 1.0f} },
        { {       w, -h*0.5f - reflH, d*0.5f}, {0,0,1}, {1.0f, 1.0f} },
        { {       w, -h*0.5f,         d*0.5f}, {0,0,1}, {1.0f, 0.0f} },
        { {-0.0006f, -h*0.5f,         d*0.5f}, {0,0,1}, {0.0f, 0.0f} },

        // Quad 1: Отражение боковой грани/корешка (-X, aNormal = {0, 1, 0} -> vNormal.y == 1.0)
        { {0.0f, -h*0.5f - reflH, -d*0.5f},           {0,1,0}, {0.0f, 1.0f} },
        { {0.0f, -h*0.5f - reflH,  d*0.5f + 0.0006f}, {0,1,0}, {1.0f, 1.0f} },
        { {0.0f, -h*0.5f,          d*0.5f + 0.0006f}, {0,1,0}, {1.0f, 0.0f} },
        { {0.0f, -h*0.5f,         -d*0.5f},           {0,1,0}, {0.0f, 0.0f} }
    };
    std::vector<uint32_t> reflInds = {
        0, 1, 2, 0, 2, 3,  // Quad 0: Обложка
        4, 5, 6, 4, 6, 7   // Quad 1: Боковой корешок
    };

    glGenVertexArrays(1, &m_reflVAO);
    glGenBuffers(1, &m_reflVBO);
    glGenBuffers(1, &m_reflEBO);
    glBindVertexArray(m_reflVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_reflVBO);
    glBufferData(GL_ARRAY_BUFFER, reflVerts.size() * sizeof(Vertex), reflVerts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_reflEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, reflInds.size() * sizeof(uint32_t), reflInds.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)12);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)24);
    glEnableVertexAttribArray(2);
}

void Renderer::init() {
    glEnable(GL_MULTISAMPLE);
    compileShaders();
    buildGeometry();
    resize(1380, 800);
}

glm::vec2 Renderer::getBoxBottomScreenPos(int slotIndex, float hoverWeight, float shelfScroll) const {
    glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    float xPos = kStartX + (static_cast<float>(slotIndex) * kSpacing) - shelfScroll;
    float curY = hoverWeight * 0.22f;

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

glm::vec4 Renderer::getInspectBoxScreenRect() const {
    glm::vec4 viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));

    // Целевая матрица развёрнутого на 180 градусов центрального бокса (задней стороной к камере)
    glm::mat4 m(1.0f);
    m = glm::translate(m, kInspectCenter);
    m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::scale(m, glm::vec3(kInspectTargetScale));
    m = glm::translate(m, glm::vec3(-kBoxW * 0.5f, 0.0f, 0.0f));

    // Проецируем углы задней крышки полиграфического вкладыша
    float padX = kBoxW * 0.025f;
    float padY = kBoxH * 0.025f;
    glm::vec3 pTopLeft  = glm::vec3(m * glm::vec4(kBoxW - padX,  kBoxH * 0.5f - padY, -kBoxD * 0.5f, 1.0f));
    glm::vec3 pBotRight = glm::vec3(m * glm::vec4(padX,         -kBoxH * 0.5f + padY, -kBoxD * 0.5f, 1.0f));

    glm::vec3 sTL = glm::project(pTopLeft, m_view, m_proj, viewport);
    glm::vec3 sBR = glm::project(pBotRight, m_view, m_proj, viewport);

    float minX = std::min(sTL.x, sBR.x);
    float maxX = std::max(sTL.x, sBR.x);
    float minY = std::min(static_cast<float>(m_height) - sTL.y, static_cast<float>(m_height) - sBR.y);
    float maxY = std::max(static_cast<float>(m_height) - sTL.y, static_cast<float>(m_height) - sBR.y);

    return glm::vec4(minX, minY, maxX, maxY);
}

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
                           float inspectProgress,
                           float inspectFrontFlip,
                           float pulseTime,
                           const glm::vec3& themeAccent) {
    if (m_aaMode == AAMode::MSAA && m_msaaFBO) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
    } else if (m_sceneFBO) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFBO);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glViewport(0, 0, m_width, m_height);
    // Очищаем только буфер глубины (фон полностью перекрывает цвет без необходимости повторной заливки)
    glClear(GL_DEPTH_BUFFER_BIT);

    // 0. Отрисовка студийного фона без multisample rasterization (у полноэкранного квада нет внутренних диагональных рёбер)
    glDisable(GL_MULTISAMPLE);
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

    if (m_aaMode == AAMode::MSAA) {
        glEnable(GL_MULTISAMPLE);
    } else {
        glDisable(GL_MULTISAMPLE);
    }

    if (!visibleIndices.empty()) {
        int inspectTrackIdx = (activePlayingIndex >= 0 && activePlayingIndex < static_cast<int>(playlist.size()))
            ? activePlayingIndex
            : 0;
    float shelfDimOpacity = 1.0f - inspectProgress * 0.62f;
    float shelfPushBackZ = -inspectProgress * 0.65f;

    // 1. Двухслойные тени под видимыми кейсами
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUseProgram(m_shadowShader);
    glUniformMatrix4fv(m_shLocProj, 1, GL_FALSE, glm::value_ptr(m_proj));
    glUniformMatrix4fv(m_shLocView, 1, GL_FALSE, glm::value_ptr(m_view));
    glBindVertexArray(m_shadowVAO);

    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        int realIdx = visibleIndices[slot];
        if (inspectProgress > 0.01f && realIdx == inspectTrackIdx) continue;

        float xPos = kStartX + (static_cast<float>(slot) * kSpacing) - shelfScroll;
        if (xPos < -2.85f || xPos > 2.45f) continue;

        float hW = (realIdx < static_cast<int>(hoverWeights.size())) ? hoverWeights[realIdx] : 0.0f;

        bool isGlow = (realIdx == activePlayingIndex || realIdx == rouletteHighlightIndex);
        glm::vec3 shadowCol = isGlow
            ? (playlist[realIdx].accentColor * 0.42f)
            : glm::vec3(0.008f, 0.010f, 0.016f);
        glUniform3fv(m_shLocColor, 1, glm::value_ptr(shadowCol));
        glUniform1f(m_shLocLift, hW + inspectProgress * 0.5f);

        glm::mat4 sm = glm::translate(glm::mat4(1.0f), glm::vec3(xPos + 0.50f, -0.68f, -0.46f + shelfPushBackZ));
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

    auto drawClosedCase = [&](int trackIdx, const glm::mat4& modelMat, float opacity, float highlight, bool includeBackFace = false, bool drawReflection = true) {
        glUniform1f(m_caseLocOpacity, opacity);
        glUniform1f(m_caseLocHighlight, highlight);
        glUniform3fv(m_caseLocAccent, 1, glm::value_ptr(playlist[trackIdx].accentColor));
        glUniformMatrix4fv(m_caseLocModel, 1, GL_FALSE, glm::value_ptr(modelMat));

        if (opacity < 0.999f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        glBindVertexArray(m_boxVAO);

        // 1) Боковая грань (процедурный минималистичный корешек)
        glUniform1i(m_caseLocType, 1);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(6 * sizeof(uint32_t)));

        // 2) Задняя крышка бокса (Back Inlay) — рисуем ТОЛЬКО когда бокс развёрнут в режиме осмотра!
        // На полке она повернута на 45° от камеры и физически полностью скрыта непрозрачным корпусом.
        if (includeBackFace) {
            glUniform1i(m_caseLocType, 5);
            glBindTexture(GL_TEXTURE_2D, playlist[trackIdx].coverTex);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(12 * sizeof(uint32_t)));
        }

        // 3) Остальные торцы пластикового лотка (Right, Top, Bottom)
        glUniform1i(m_caseLocType, 0);
        glUniform3f(m_caseLocBaseColor, 0.15f, 0.17f, 0.22f);
        glDrawElements(GL_TRIANGLES, 18, GL_UNSIGNED_INT, (void*)(18 * sizeof(uint32_t)));

        // 4) Лицевая обложка
        glUniform1i(m_caseLocType, 2);
        glBindTexture(GL_TEXTURE_2D, playlist[trackIdx].coverTex);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(0 * sizeof(uint32_t)));

        // 4.1) Нежное матовое 3D-отражение (обложка + боковая грань) в лакированном полу (без сглаживания и 0% лишней нагрузки)
        if (m_reflectionsEnabled && drawReflection && m_reflVAO != 0) {
            glDisable(GL_MULTISAMPLE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glUniform1i(m_caseLocType, 6);
            glBindVertexArray(m_reflVAO);
            glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_INT, (void*)0);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            if (m_aaMode == AAMode::MSAA) {
                glEnable(GL_MULTISAMPLE);
            }
        }

        // 5) Плашка с названием и исполнителем внизу лицевой обложки
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1i(m_caseLocType, 4);
        glBindTexture(GL_TEXTURE_2D, playlist[trackIdx].badgeTex);
        glBindVertexArray(m_badgeVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        // 6) Акриловый глянец поверх лицевой стороны — минимальный зазор (0.4мм), полностью исключающий щель
        glDepthMask(GL_FALSE);
        glUniform1i(m_caseLocType, 3);
        glm::mat4 glassM = glm::translate(modelMat, glm::vec3(0.0f, 0.0f, 0.0004f));
        glUniformMatrix4fv(m_caseLocModel, 1, GL_FALSE, glm::value_ptr(glassM));
        glBindVertexArray(m_boxVAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (void*)(0 * sizeof(uint32_t)));
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    };

    int flyingSlot = -1;
    int inspectSlot = -1;

    // 2. Рендер видимых CD-боксов на полке
    for (size_t slot = 0; slot < visibleIndices.size(); ++slot) {
        int realIdx = visibleIndices[slot];
        if (realIdx == selectedIndex) {
            flyingSlot = static_cast<int>(slot);
        }
        if (realIdx == inspectTrackIdx) {
            inspectSlot = static_cast<int>(slot);
            if (inspectProgress > 0.005f) continue; // Этот бокс вылетает на передний план!
        }

        float xPos = kStartX + (static_cast<float>(slot) * kSpacing) - shelfScroll;
        if (xPos < -2.85f || xPos > 2.45f) continue;

        float hW = (realIdx < static_cast<int>(hoverWeights.size())) ? hoverWeights[realIdx] : 0.0f;
        float curY = hW * 0.22f;
        if (realIdx == activePlayingIndex) {
            curY += std::abs(std::sin(pulseTime * 3.2f)) * 0.022f;
        }

        float highlight = (realIdx == rouletteHighlightIndex) ? 1.0f : (hW * 0.45f);

        glm::mat4 baseM(1.0f);
        baseM = glm::translate(baseM, glm::vec3(xPos, curY, shelfPushBackZ));
        baseM = glm::rotate(baseM, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        drawClosedCase(realIdx, baseM, shelfDimOpacity, highlight);
    }

    // 3. Анимация плавного перетекания выбранного бокса в нижний бар плеера (если не открыт режим просмотра оборота)
    if (inspectProgress < 0.01f &&
        selectedIndex >= 0 && selectedIndex < static_cast<int>(playlist.size()) &&
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

        drawClosedCase(selectedIndex, flyM, alpha, 0.6f, false, false);
    }

    // 4. КИНЕМАТОГРАФИЧНЫЙ 3D-ВЫЛЕТ И РАЗВОРОТ БОКСА НА 180° ЗАДНЕЙ СТОРОНОЙ К ЗРИТЕЛЮ (Режим Lyrics / Караоке)
    if (inspectProgress > 0.005f && inspectTrackIdx >= 0 && inspectTrackIdx < static_cast<int>(playlist.size())) {
        float t = std::clamp(inspectProgress, 0.0f, 1.0f);
        // Плавная S-кривая (cubic ease-in-out)
        float ease = (t < 0.5f) ? (4.0f * t * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f);

        float shelfX = (inspectSlot >= 0)
            ? (kStartX + static_cast<float>(inspectSlot) * kSpacing - shelfScroll)
            : 0.0f;
        shelfX = std::clamp(shelfX, -1.8f, 1.8f);

        // На полке центр бокса смещён от точки опоры (0,0,0) на повёрнутый вектор (kBoxW*0.5, 0, 0)
        glm::vec3 shelfCenter(
            shelfX + (kBoxW * 0.5f) * kCos45,
            0.08f,
            -(kBoxW * 0.5f) * kCos45
        );
        glm::vec3 curCenter = glm::mix(shelfCenter, kInspectCenter, ease);
        // Лёгкая дуга вверх при вылете с полки
        curCenter.y += std::sin(ease * 3.14159265f) * 0.12f;

        // Угол разворота: с 45° на полке до 180° (задняя сторона с текстом песни)
        float targetYaw = glm::mix(180.0f, 0.0f, std::clamp(inspectFrontFlip, 0.0f, 1.0f));
        float curYaw = glm::mix(45.0f, targetYaw, ease);
        float curScale = glm::mix(1.0f, kInspectTargetScale, ease) + std::sin(ease * 3.14159265f) * 0.025f;

        // Кинематографичная тень под развёрнутым боксом в режиме осмотра
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(m_shadowShader);
        glUniformMatrix4fv(m_shLocProj, 1, GL_FALSE, glm::value_ptr(m_proj));
        glUniformMatrix4fv(m_shLocView, 1, GL_FALSE, glm::value_ptr(m_view));
        glBindVertexArray(m_shadowVAO);
        glm::vec3 shadowCol = playlist[inspectTrackIdx].accentColor * 0.32f * ease;
        glUniform3fv(m_shLocColor, 1, glm::value_ptr(shadowCol));
        glUniform1f(m_shLocLift, 0.12f * (1.0f - ease));
        glm::mat4 inspShadowM = glm::translate(glm::mat4(1.0f), glm::vec3(curCenter.x, -0.68f, curCenter.z - 0.06f));
        inspShadowM = glm::scale(inspShadowM, glm::vec3(1.18f * curScale, 1.0f, 0.35f * curScale));
        glUniformMatrix4fv(m_shLocModel, 1, GL_FALSE, glm::value_ptr(inspShadowM));
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // Восстанавливаем шейдер кейса
        glUseProgram(m_caseShader);
        glUniformMatrix4fv(m_caseLocProj, 1, GL_FALSE, glm::value_ptr(m_proj));
        glUniformMatrix4fv(m_caseLocView, 1, GL_FALSE, glm::value_ptr(m_view));
        glUniform3fv(m_caseLocCamPos, 1, glm::value_ptr(m_camPos));
        glUniform1i(m_caseLocTex, 0);
        glActiveTexture(GL_TEXTURE0);

        glm::mat4 inspM(1.0f);
        inspM = glm::translate(inspM, curCenter);
        inspM = glm::rotate(inspM, glm::radians(curYaw), glm::vec3(0.0f, 1.0f, 0.0f));
        inspM = glm::scale(inspM, glm::vec3(curScale));
        inspM = glm::translate(inspM, glm::vec3(-kBoxW * 0.5f, 0.0f, 0.0f));

        drawClosedCase(inspectTrackIdx, inspM, 1.0f, 0.35f * ease, true, false);
    }
    }

    glDisable(GL_CULL_FACE);

    // Вывод в экранный Framebuffer 0: через аппаратный MSAA resolve, прямой аппаратный blit или FXAA
    if (m_aaMode == AAMode::MSAA && m_msaaFBO) {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else if (m_aaMode == AAMode::Off && m_sceneFBO) {
        // Прямой 1:1 попиксельный аппаратный перенос без шейдеров и билинейных искажений
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else if (m_sceneFBO) {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);

        glUseProgram(m_fxaaShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_sceneColorTex);
        glUniform1i(m_fxaaLocTex, 0);
        glUniform2f(m_fxaaLocRcpFrame, 1.0f / static_cast<float>(m_width), 1.0f / static_cast<float>(m_height));

        glBindVertexArray(m_bgVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }
}