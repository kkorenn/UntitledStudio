cmake_minimum_required(VERSION 3.22)
project(UntitledStudio VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(libs/JUCE)

juce_add_gui_app(UntitledStudio
    PRODUCT_NAME "untitledstudio"
    COMPANY_NAME "untitledstudio")

target_sources(UntitledStudio PRIVATE Source/Main.cpp)

target_compile_definitions(UntitledStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:UntitledStudio,JUCE_VERSION>")

target_link_libraries(UntitledStudio
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
