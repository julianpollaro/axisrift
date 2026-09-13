cmake_minimum_required(VERSION 3.22)

# Mac: universal for Logic on Apple Silicon. Ignored on Win/Linux.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AxisRift VERSION 1.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 7.0.12
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# ---- formats: AU only on Apple; VST3 everywhere (FL Studio, REAPER, Bitwig, etc.)
set(AR_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND AR_FORMATS AU)
endif()

function(axisrift_plugin target code product sources)
  juce_add_plugin(${target}
    COMPANY_NAME "Axis Rift"
    BUNDLE_ID com.axisrift.${target}
    PLUGIN_MANUFACTURER_CODE Axrf
    PLUGIN_CODE ${code}
    FORMATS ${AR_FORMATS}
    PRODUCT_NAME "${product}"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(${target} PRIVATE ${sources})
  target_compile_definitions(${target} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
  target_link_libraries(${target} PRIVATE
    juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()

axisrift_plugin(StubbsCue Stbq "StubbsCue" src/StubbsCue.cpp)
axisrift_plugin(AutoQuacker Atqk "AutoQuacker" src/AutoQuacker.cpp)

axisrift_plugin(PearlVox Prlv "PearlVox" src/PearlVox.cpp)
