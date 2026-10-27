cmake_minimum_required(VERSION 3.22)
project(DZODArranger VERSION 0.2.1 LANGUAGES C CXX)
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
  core/Arranger.cpp core/Match.cpp
  third_party/stemmer/DzodStemmer.cpp third_party/stemmer/Mp3Writer.cpp)

add_executable(ArrangerTool tools/ArrangerTool.cpp ${ARRANGER_CORE})
set_target_properties(ArrangerTool PROPERTIES OUTPUT_NAME DZODArrangerTool)
target_link_libraries(ArrangerTool PRIVATE DZODShine DZODJuceAudio)
