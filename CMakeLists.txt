cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(JewelPlayer CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. GLFW
FetchContent_Declare(
        glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# 2. GLM
FetchContent_Declare(
        glm
        URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

# 3. miniaudio
FetchContent_Declare(
        miniaudio
        URL https://github.com/mackron/miniaudio/archive/refs/heads/master.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniaudio)

# 4. GLAD
FetchContent_Declare(
        glad
        URL https://github.com/libigl/libigl-glad/archive/refs/heads/master.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glad)

# 5. Dear ImGui
FetchContent_Declare(
        imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.4.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

# 6. stb (stb_image)
FetchContent_Declare(
        stb
        URL https://github.com/nothings/stb/archive/refs/heads/master.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(stb)

add_executable(JewelPlayer WIN32
        src/AudioEngine.cpp
        src/Metadata.cpp
        src/Renderer.cpp
        src/main.cpp
        ${glad_SOURCE_DIR}/src/glad.c
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        src/miniaudio.h
)

target_include_directories(JewelPlayer PRIVATE
        src
        ${miniaudio_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${glad_SOURCE_DIR}/include
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(JewelPlayer PRIVATE
        glfw
        glm::glm
        opengl32
        comdlg32
        gdi32
        shell32
        ole32
        dwmapi
        winmm
)