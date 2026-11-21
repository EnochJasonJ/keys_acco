cmake_minimum_required(VERSION 3.22)
project(MidiArranger VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(JUCE
    URL https://codeload.github.com/juce-framework/JUCE/tar.gz/refs/tags/8.0.12
    URL_HASH SHA256=dec1a8baee5aaec4502b717a421b850e257ab7e9c2efd03766185ec1957cf102)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MidiArranger PRODUCT_NAME "MIDI Arranger"
    BUNDLE_ID com.midiarranger.desktop)
target_sources(MidiArranger PRIVATE src/Main.cpp src/Arranger.cpp)
target_compile_definitions(MidiArranger PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MidiArranger PRIVATE
    juce::juce_gui_basics juce::juce_audio_devices juce::juce_recommended_config_flags)
