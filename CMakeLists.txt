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
        src/LyricsFetcher.cpp
        src/Renderer.cpp
        src/main.cpp
        ${glad_SOURCE_DIR}/src/glad.c
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        src/LyricsFetcher.hpp
        src/LyricsFetcher.cpp
        src/CoverFetcher.hpp
        src/CoverFetcher.cpp
        src/SmtcManager.hpp
        src/SmtcManager.cpp
        src/YandexMusicManager.hpp
        src/YandexMusicManager.cpp
)

# В Debug копируем ym_bridge.py для удобной отладки в IDE
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ym_bridge.py ${CMAKE_CURRENT_BINARY_DIR}/ym_bridge.py COPYONLY)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dist/ym_bridge.exe")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/dist/ym_bridge.exe" "${CMAKE_CURRENT_BINARY_DIR}/ym_bridge.exe" COPYONLY)
endif()
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
        winhttp
        psapi
        windowsapp
)

if(MSVC)
    target_link_options(JewelPlayer PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()

# В Release-сборке автоматически формируем чистую папку для релиза (без ninja/cmake мусора)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RELEASE_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}/publish")
    set(ROOT_RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")

    add_custom_command(TARGET JewelPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:JewelPlayer>" "${RELEASE_DIST_DIR}/JewelPlayer.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/dist/ym_bridge.exe" "${RELEASE_DIST_DIR}/ym_bridge.exe"
        COMMAND ${CMAKE_COMMAND} -E chdir "${RELEASE_DIST_DIR}" ${CMAKE_COMMAND} -E tar cf "JewelFlow-Release.zip" --format=zip "JewelPlayer.exe" "ym_bridge.exe"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_RELEASE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RELEASE_DIST_DIR}/JewelPlayer.exe" "${ROOT_RELEASE_DIR}/JewelPlayer.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RELEASE_DIST_DIR}/ym_bridge.exe" "${ROOT_RELEASE_DIR}/ym_bridge.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RELEASE_DIST_DIR}/JewelFlow-Release.zip" "${ROOT_RELEASE_DIR}/JewelFlow-Release.zip"
        COMMENT "Packaging clean release into ${RELEASE_DIST_DIR} and ${ROOT_RELEASE_DIR} (JewelPlayer.exe, ym_bridge.exe, JewelFlow-Release.zip)"
    )
endif()

