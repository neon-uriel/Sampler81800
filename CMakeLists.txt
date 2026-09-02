cmake_minimum_required(VERSION 3.22)

project(OtoMadSampler VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# =====================================================================
#  依存の取得 (FetchContent)  — DESIGN.md §0.1
# =====================================================================
include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15          # JUCE 8。バージョンを上げるときはここだけ変える
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# signalsmith-stretch（MIT・ヘッダオンリー）— StretchLibEngine の実体。DESIGN.md §4.6。
# 自作エンジン（WSOLA / Phase Vocoder / Granular）との音質比較の基準にもなる。
# タグは 1.1.0 までしか無く、必要な API は それ以降（ヘッダ内 version は 1.3.2）なので
# 検証したコミットに固定する。main のままだと CI が別のコミットを引きうる。
# SHA 指定と GIT_SHALLOW は併用できないので shallow は付けない。
FetchContent_Declare(SignalsmithStretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        57b93f4e9206a089a45387eaa39bdc9f310d3308)
FetchContent_MakeAvailable(SignalsmithStretch)

# =====================================================================
#  プラグイン本体
# =====================================================================
juce_add_plugin(OtoMadSampler
    PRODUCT_NAME                "OtoMadSampler"
    COMPANY_NAME                "neon-uriel"
    BUNDLE_ID                   "com.neonuriel.otomadsampler"
    PLUGIN_MANUFACTURER_CODE    Nuri              # 4文字・先頭大文字1つ
    PLUGIN_CODE                 Otms              # 4文字・一意
    FORMATS                     VST3 Standalone   # CLAP は Phase 6 (DESIGN.md §7)
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    NEEDS_WEBVIEW2              TRUE   # Web UI (試作): WebView2 を静的リンク
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Instrument" "Sampler")

target_sources(OtoMadSampler PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/WebEditor.cpp
    src/core/SampleLoader.cpp
    src/core/SourceReader.cpp
    src/core/Voice.cpp
    src/core/VoiceManager.cpp
    src/core/PortamentoGenerator.cpp
    src/core/GlideMatcher.cpp
    src/pitch/VarispeedEngine.cpp
    src/pitch/WsolaEngine.cpp
    src/pitch/PhaseVocoderEngine.cpp
    src/pitch/GranularEngine.cpp
    src/pitch/StretchLibEngine.cpp
    src/pitch/ReaperPitchShiftEngine.cpp
    src/core/FfmpegDecoder.cpp
    src/core/PitchDetect.cpp
    src/core/PitchFlattener.cpp
    src/pitch/PitchCache.cpp
    src/pitch/ElastiqueDirect.cpp
    src/pitch/EngineResources.cpp
    src/pitch/dsp/Fft.cpp
    src/host/ReaperApi.cpp
    src/gui/DropZone.cpp
    src/gui/WaveformView.cpp)

target_include_directories(OtoMadSampler PRIVATE src)

# JUCE のフロントエンド JS（native interop）は「使っている JUCE から」configure 時に取り出す。
# リポジトリに手でコピーすると、JUCE のタグを上げたとき C++ 側のプロトコルだけが変わって
# JS が取り残される（構造的に検知できない）。ここでコピーすればバージョンずれが起こらない。
set(OTOMAD_WEBUI_JS_DIR "${CMAKE_BINARY_DIR}/webui_js")
file(MAKE_DIRECTORY "${OTOMAD_WEBUI_JS_DIR}")
file(COPY
        "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/index.js"
        "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript/check_native_interop.js"
     DESTINATION "${OTOMAD_WEBUI_JS_DIR}")
file(RENAME "${OTOMAD_WEBUI_JS_DIR}/index.js" "${OTOMAD_WEBUI_JS_DIR}/juce-index.js")

# ロゴ等の埋め込みリソース → BinaryData::logo_png
# Web UI (試作) のアセットも埋め込み、実行時に WebBrowserComponent へローカル配信する。
juce_add_binary_data(OtoMadSamplerAssets SOURCES
    resources/logo.png
    resources/webui/index.html
    resources/webui/style.css
    resources/webui/app.js
    ${OTOMAD_WEBUI_JS_DIR}/juce-index.js
    ${OTOMAD_WEBUI_JS_DIR}/check_native_interop.js)   # juce-index.js が import するので必須
target_link_libraries(OtoMadSampler PRIVATE OtoMadSamplerAssets)

target_compile_features(OtoMadSampler PRIVATE cxx_std_20)

# ソースは UTF-8。MSVC は既定でシステムコードページ(日本語環境ではCP932)として解釈し、
# 日本語コメント末尾の 0x5C が行継続扱いになって構文が壊れる。/utf-8 で明示する。
target_compile_options(OtoMadSampler PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_compile_definitions(OtoMadSampler PUBLIC
    OTOMAD_VERSION="${PROJECT_VERSION}"          # アップデート確認用の自バージョン
    # Web UI (feature/web-ui): WebBrowserComponent + WebView2 を有効化
    # OTOMAD_WEB_UI=0 にすればネイティブ版エディタに戻せる（比較用）。
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    OTOMAD_WEB_UI=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # JUCE_DISPLAY_SPLASH_SCREEN=0 は商用ライセンス時のみ可 (DESIGN.md §0.3)。
    # 無償(AGPLv3)枠では既定(スプラッシュ表示)のままにすること。
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(OtoMadSampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_vst3_headers   # VST3 SDK ヘッダ（REAPER連携の IHostApplication 取得用, §5.2）
        signalsmith-stretch       # StretchLibEngine（MIT・ヘッダオンリー, §4.6）
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# =====================================================================
#  テスト (Catch2)  — DESIGN.md §8
# =====================================================================
enable_testing()
add_subdirectory(tests)
