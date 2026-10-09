cmake_minimum_required(VERSION 3.22)
project(UntitledStudio VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

# Windows: static MSVC runtime (like Tracktion's examples), so the app runs without the VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Windows: WASAPI and DirectSound are always on. ASIO needs Steinberg's SDK (not redistributable):
# cmake -DUNTITLEDSTUDIO_ASIO_SDK_DIR=C:/SDKs/asiosdk ...
set(UNTITLEDSTUDIO_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder (Windows only); enables ASIO when set")

# Update check: URL of a small JSON feed ({ "version": "x.y.z", "url": "...", "notes": "..." }). Empty = no update checks.
set(UNTITLEDSTUDIO_UPDATE_URL "" CACHE STRING "Update feed URL (empty disables the update check)")

add_subdirectory(libs/JUCE)
add_subdirectory(libs/tracktion_engine/modules)

juce_add_gui_app(UntitledStudio
    PRODUCT_NAME "Untitled Studio"
    COMPANY_NAME "Untitled Studio"
    BUNDLE_ID "com.untitledstudio.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Audio recording"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon_1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon_1024.png"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU TRUE)

target_sources(UntitledStudio PRIVATE Source/Main.cpp)

juce_add_binary_data(UntitledStudioData SOURCES
    resources/icon/icon_1024.png
    THIRD_PARTY_LICENSES.md)

# License texts shipped inside the app (Resources/licenses), required for binary distribution.
set(license_dir "${CMAKE_CURRENT_BINARY_DIR}/licenses")
set(JM libs/JUCE/modules)
set(TE libs/tracktion_engine/modules)
foreach(entry
        "LICENSE-UntitledStudio-GPLv3.txt|LICENSE"
        "THIRD_PARTY_LICENSES.md|THIRD_PARTY_LICENSES.md"
        "JUCE.md|libs/JUCE/LICENSE.md"
        "JUCE.spdx.json|libs/JUCE/JUCE.spdx.json"
        "TracktionEngine.md|libs/tracktion_engine/LICENSE.md"
        "zlib.txt|${JM}/juce_core/zip/zlib/LICENSE"
        "HarfBuzz.txt|${JM}/juce_graphics/fonts/harfbuzz/COPYING"
        "SheenBidi.txt|${JM}/juce_graphics/unicode/sheenbidi/LICENSE"
        "LunaSVG.txt|${JM}/juce_graphics/drawables/lunasvg/LICENSE"
        "PlutoVG.txt|${JM}/juce_graphics/drawables/lunasvg/plutovg/LICENSE"
        "libpng.txt|${JM}/juce_graphics/image_formats/pnglib/LICENSE"
        "libjpeg-README.txt|${JM}/juce_graphics/image_formats/jpglib/README"
        "libwebp.txt|${JM}/juce_graphics/image_formats/libwebp/COPYING"
        "FLAC.txt|${JM}/juce_audio_formats/codecs/flac/Flac Licence.txt"
        "libogg.txt|${JM}/juce_audio_formats/codecs/ogg/COPYING"
        "libvorbis.txt|${JM}/juce_audio_formats/codecs/vorbis/COPYING"
        "Opus.txt|${JM}/juce_audio_formats/codecs/opus/opus/COPYING"
        "opusfile.txt|${JM}/juce_audio_formats/codecs/opus/opusfile/COPYING"
        "libopusenc.txt|${JM}/juce_audio_formats/codecs/opus/libopusenc/COPYING"
        "VST3_SDK.txt|${JM}/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt"
        "CHOC.md|${TE}/3rd_party/choc/LICENSE.md"
        "crill.txt|${TE}/3rd_party/crill/LICENSE.txt"
        "TinySoundFont.txt|libs/TinySoundFont/LICENSE")
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 name)
    list(GET pair 1 source)
    configure_file("${source}" "${license_dir}/${name}" COPYONLY)
endforeach()
juce_add_bundle_resources_directory(UntitledStudio "${license_dir}")

target_compile_definitions(UntitledStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_AIR_WINDOWS=1
    JUCE_USE_LAME_AUDIO_FORMAT=1
    UNTITLEDSTUDIO_UPDATE_URL="${UNTITLEDSTUDIO_UPDATE_URL}"
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_VERSION>")

if(APPLE)
    target_compile_options(UntitledStudio PRIVATE "-fno-aligned-allocation")
elseif(MSVC)
    target_compile_options(UntitledStudio PRIVATE /bigobj /MP)
endif()

if(WIN32 AND UNTITLEDSTUDIO_ASIO_SDK_DIR)
    if(NOT EXISTS "${UNTITLEDSTUDIO_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "UNTITLEDSTUDIO_ASIO_SDK_DIR has no common/iasiodrv.h: ${UNTITLEDSTUDIO_ASIO_SDK_DIR}")
    endif()
    target_compile_definitions(UntitledStudio PRIVATE JUCE_ASIO=1)
    target_include_directories(UntitledStudio PRIVATE "${UNTITLEDSTUDIO_ASIO_SDK_DIR}/common")
endif()

target_link_libraries(UntitledStudio
    PRIVATE
        UntitledStudioData
        tracktion::tracktion_core
        tracktion::tracktion_engine
        tracktion::tracktion_graph
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
