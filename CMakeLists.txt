cmake_minimum_required(VERSION 3.22)
project(DZODArranger VERSION 0.7.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 8 (только модули звука, без окна): -DJUCE_DIR=<путь к исходникам JUCE>, иначе скачивается.
# Модули собираются напрямую, без juceaide — так ядро собирается и на машине без X11.
if(NOT JUCE_DIR)
  include(FetchContent)
  FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW ON)
  FetchContent_Populate(JUCE)
  set(JUCE_DIR ${juce_SOURCE_DIR})
endif()
set(JM ${JUCE_DIR}/modules)
add_library(DZODJuceAudio STATIC
  ${JM}/juce_core/juce_core.cpp ${JM}/juce_core/juce_core_CompilationTime.cpp ${JM}/juce_audio_basics/juce_audio_basics.cpp
  ${JM}/juce_audio_formats/juce_audio_formats.cpp ${JM}/juce_dsp/juce_dsp.cpp)
target_include_directories(DZODJuceAudio PUBLIC ${JM})
target_compile_definitions(DZODJuceAudio PUBLIC
  JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
  JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_DSP_USE_INTEL_MKL=0
  JUCE_MODULE_AVAILABLE_juce_core=1 JUCE_MODULE_AVAILABLE_juce_audio_basics=1
  JUCE_MODULE_AVAILABLE_juce_audio_formats=1 JUCE_MODULE_AVAILABLE_juce_dsp=1
  $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1> $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>)
set_target_properties(DZODJuceAudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_link_libraries(DZODJuceAudio PUBLIC winmm ws2_32 version shlwapi wininet)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(DZODJuceAudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  target_compile_options(DZODJuceAudio PUBLIC /bigobj /utf-8)
endif()

# кодировщик MP3 shine (нужен Stemmer core)
file(GLOB SHINE_SRC third_party/shine/*.c)
add_library(DZODShine STATIC ${SHINE_SRC})
target_include_directories(DZODShine PUBLIC third_party/shine)
set_target_properties(DZODShine PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ядро Arranger (+ Stemmer core) без окна
set(ARRANGER_CORE
  core/Arranger.cpp core/Match.cpp core/Stretch.cpp core/Phase.cpp
  third_party/stemmer/DzodStemmer.cpp third_party/stemmer/Mp3Writer.cpp)

add_executable(ArrangerTool tools/ArrangerTool.cpp ${ARRANGER_CORE})
# Signalsmith Stretch (MIT) + Signalsmith Linear (MIT) — только заголовки
target_include_directories(ArrangerTool PRIVATE third_party/signalsmith/stretch/include third_party/signalsmith/linear/include)
set_target_properties(ArrangerTool PROPERTIES OUTPUT_NAME DZODArrangerTool)
target_link_libraries(ArrangerTool PRIVATE DZODShine DZODJuceAudio)

# ---------------------------------------------------------------------------
# DZOD Arranger.exe — окно (JUCE gui + audio devices), модули собираются напрямую, без juceaide
option(DZOD_BUILD_APP "Build the DZOD Arranger window app" ON)
set(DZOD_X11_STUBS "" CACHE PATH "Linux: folder with stub X11 headers (Xrandr/Xinerama/Xcursor) for a compile check")
if(DZOD_BUILD_APP)
  add_library(DZODJuceGui STATIC
    ${JM}/juce_core/juce_core.cpp ${JM}/juce_core/juce_core_CompilationTime.cpp ${JM}/juce_events/juce_events.cpp
    ${JM}/juce_data_structures/juce_data_structures.cpp ${JM}/juce_graphics/juce_graphics.cpp
    ${JM}/juce_graphics/juce_graphics_Harfbuzz.cpp ${JM}/juce_graphics/juce_graphics_Sheenbidi.c
    ${JM}/juce_gui_basics/juce_gui_basics.cpp
    ${JM}/juce_audio_basics/juce_audio_basics.cpp ${JM}/juce_audio_formats/juce_audio_formats.cpp
    ${JM}/juce_audio_devices/juce_audio_devices.cpp ${JM}/juce_dsp/juce_dsp.cpp)
  target_include_directories(DZODJuceGui PUBLIC ${JM})
  target_compile_definitions(DZODJuceGui PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
    JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_DSP_USE_INTEL_MKL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODULE_AVAILABLE_juce_core=1 JUCE_MODULE_AVAILABLE_juce_events=1 JUCE_MODULE_AVAILABLE_juce_data_structures=1
    JUCE_MODULE_AVAILABLE_juce_graphics=1 JUCE_MODULE_AVAILABLE_juce_gui_basics=1 JUCE_MODULE_AVAILABLE_juce_audio_basics=1
    JUCE_MODULE_AVAILABLE_juce_audio_formats=1 JUCE_MODULE_AVAILABLE_juce_audio_devices=1 JUCE_MODULE_AVAILABLE_juce_dsp=1
    $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1> $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>)
  if(WIN32)
    target_link_libraries(DZODJuceGui PUBLIC winmm ws2_32 version shlwapi wininet imm32 comdlg32 ole32 oleaut32 shell32 uuid user32 gdi32 dwrite d2d1 dxgi d3d11 windowscodecs)
  elseif(APPLE)
    target_link_libraries(DZODJuceGui PUBLIC "-framework Cocoa" "-framework CoreAudio" "-framework CoreMIDI" "-framework AudioToolbox" "-framework Accelerate" "-framework IOKit" "-framework QuartzCore" "-framework Metal")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FT REQUIRED freetype2 fontconfig)
    target_include_directories(DZODJuceGui PUBLIC ${FT_INCLUDE_DIRS})
    if(DZOD_X11_STUBS)
      target_include_directories(DZODJuceGui PUBLIC ${DZOD_X11_STUBS})
    endif()
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
      target_link_libraries(DZODJuceGui PUBLIC ALSA::ALSA)
    else()
      target_compile_definitions(DZODJuceGui PUBLIC JUCE_ALSA=0)
    endif()
    target_compile_definitions(DZODJuceGui PUBLIC JUCE_JACK=0)
    target_link_libraries(DZODJuceGui PUBLIC ${FT_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS} rt)
  endif()
  if(MSVC)
    target_compile_options(DZODJuceGui PUBLIC /bigobj /utf-8)
  endif()

  # шрифты дизайн-системы DZOD (PT Sans, OFL) — в программу
  set(FONT_FILES "PTSansRegular=${CMAKE_CURRENT_SOURCE_DIR}/app/fonts/PT_Sans-Web-Regular.ttf;PTSansBold=${CMAKE_CURRENT_SOURCE_DIR}/app/fonts/PT_Sans-Web-Bold.ttf;PTSansNarrowRegular=${CMAKE_CURRENT_SOURCE_DIR}/app/fonts/PT_Sans-Narrow-Web-Regular.ttf;PTSansNarrowBold=${CMAKE_CURRENT_SOURCE_DIR}/app/fonts/PT_Sans-Narrow-Web-Bold.ttf")
  set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${GEN})
  add_custom_command(OUTPUT ${GEN}/AppFonts.cpp ${GEN}/AppFonts.h
    COMMAND ${CMAKE_COMMAND} "-DOUT=${GEN}/AppFonts.cpp" "-DHDR=${GEN}/AppFonts.h" -DHDR_NAME=AppFonts.h -DNS=dzod::ui::fonts "-DFILES=${FONT_FILES}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake ${CMAKE_CURRENT_SOURCE_DIR}/app/fonts/PT_Sans-Web-Regular.ttf
    COMMENT "Embedding fonts" VERBATIM)

  add_executable(ArrangerApp WIN32
    app/Main.cpp app/ArrangerComponent.cpp app/TableView.cpp app/Player.cpp app/Theme.cpp ${GEN}/AppFonts.cpp ${ARRANGER_CORE})
  if(WIN32)
    target_sources(ArrangerApp PRIVATE app/resources/app.rc)
  endif()
  target_include_directories(ArrangerApp PRIVATE ${GEN} third_party/signalsmith/stretch/include third_party/signalsmith/linear/include)
  target_compile_definitions(ArrangerApp PRIVATE DZOD_ARRANGER_VERSION="${PROJECT_VERSION}")
  set_target_properties(ArrangerApp PROPERTIES OUTPUT_NAME "DZOD Arranger")
  target_link_libraries(ArrangerApp PRIVATE DZODShine DZODJuceGui)
endif()
