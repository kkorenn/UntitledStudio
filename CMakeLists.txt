cmake_minimum_required(VERSION 3.22)
project(UntitledStudio VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

add_subdirectory(libs/JUCE)
add_subdirectory(libs/tracktion_engine/modules)

juce_add_gui_app(UntitledStudio
    PRODUCT_NAME "untitledstudio"
    COMPANY_NAME "untitledstudio"
    BUNDLE_ID "com.untitledstudio.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Audio recording"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
    PLUGINHOST_AU TRUE)

target_sources(UntitledStudio PRIVATE Source/Main.cpp)

target_compile_definitions(UntitledStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TRACKTION_AIR_WINDOWS=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_VERSION>")

if(APPLE)
    target_compile_options(UntitledStudio PRIVATE "-fno-aligned-allocation")
endif()

target_link_libraries(UntitledStudio
    PRIVATE
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
