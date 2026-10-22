cmake_minimum_required(VERSION 3.16)
project(ProResPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(DEFINED ENV{QT_DIR})
    get_filename_component(QT_PREFIX "$ENV{QT_DIR}" DIRECTORY)
    list(PREPEND CMAKE_PREFIX_PATH "${QT_PREFIX}")
endif()

find_package(Qt5 5.12 REQUIRED COMPONENTS Widgets Multimedia)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rd/ffmpeg")
set(MEDIAINFO_DLL "${CMAKE_SOURCE_DIR}/3rd/MediaInfo/MediaInfo.dll")
set(MEDIAINFO_INCLUDE "${CMAKE_SOURCE_DIR}/3rd/MediaInfo/Developers/Source/MediaInfoDLL")

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "未找到 FFmpeg 头文件：${FFMPEG_ROOT}")
endif()
if(NOT EXISTS "${MEDIAINFO_DLL}")
    message(FATAL_ERROR "未找到 MediaInfo.dll：${MEDIAINFO_DLL}")
endif()

add_executable(ProResPlayer
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/VideoWidget.cpp
    src/VideoWidget.h
    src/Player.cpp
    src/Player.h
    src/MediaProbe.cpp
    src/MediaProbe.h
)

target_include_directories(ProResPlayer PRIVATE
    "${FFMPEG_ROOT}/include"
    "${MEDIAINFO_INCLUDE}"
)
target_link_directories(ProResPlayer PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(ProResPlayer PRIVATE
    Qt5::Widgets
    Qt5::Multimedia
    avcodec
    avformat
    avutil
    swscale
    swresample
    d3d11
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_options(ProResPlayer PRIVATE /utf-8 /W3)
endif()

add_custom_command(TARGET ProResPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:ProResPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MEDIAINFO_DLL}" "$<TARGET_FILE_DIR:ProResPlayer>"
)

find_program(WINDEPLOYQT_EXE windeployqt HINTS "$ENV{QT_DIR}")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET ProResPlayer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --no-translations "$<TARGET_FILE:ProResPlayer>"
    )
endif()
