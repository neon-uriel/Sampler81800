# =====================================================================
#  DSPユニットテスト (Catch2)  — DESIGN.md §8.1
#  Phase 0 では器だけ。Phase 1 以降でエンジンのパラメータ化テストを足す。
# =====================================================================
include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.15.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(otomad_tests
    test_placeholder.cpp
    test_source_reader.cpp
    test_varispeed.cpp
    test_portamento.cpp
    test_glide.cpp
    test_voice_alloc.cpp
    test_wsola.cpp
    test_phasevocoder.cpp
    test_granular.cpp
    test_stretchlib.cpp
    test_samplerate.cpp
    test_voice_engines.cpp
    test_vibrato.cpp
    test_pitchflatten.cpp
    test_pitchcache.cpp
    test_retrigger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SourceReader.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Voice.cpp
    ${CMAKE_SOURCE_DIR}/src/core/VoiceManager.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/ReaperPitchShiftEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/host/ReaperApi.cpp
    ${CMAKE_SOURCE_DIR}/src/core/PortamentoGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GlideMatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SampleLoader.cpp
    ${CMAKE_SOURCE_DIR}/src/core/PitchDetect.cpp
    ${CMAKE_SOURCE_DIR}/src/core/PitchFlattener.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/VarispeedEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/WsolaEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/PhaseVocoderEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/GranularEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/StretchLibEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/EngineResources.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/PitchCache.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/ElastiqueDirect.cpp
    ${CMAKE_SOURCE_DIR}/src/pitch/dsp/Fft.cpp)

target_include_directories(otomad_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(otomad_tests PRIVATE cxx_std_20)
target_compile_options(otomad_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
# SampleLoader::rebuildFromOriginal（SR変換）を検証するので JUCE が要る。
target_link_libraries(otomad_tests PRIVATE Catch2::Catch2WithMain signalsmith-stretch
                                          juce::juce_audio_formats
                                          juce::juce_vst3_headers)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(otomad_tests)
