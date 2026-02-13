cmake_minimum_required(VERSION 3.22)
project(SampledexChordLab VERSION 2.0.0 LANGUAGES C CXX OBJCXX)

# --- 1. Build Settings ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO")
endif()

# --- 2. JUCE Setup ---
# Default to environment variable if set
if(NOT JUCE_DIR AND DEFINED ENV{JUCE_DIR})
    set(JUCE_DIR "$ENV{JUCE_DIR}" CACHE PATH "Path to JUCE repository" FORCE)
endif()

# Fallback to standard locations
if(NOT JUCE_DIR)
    if(EXISTS "$ENV{HOME}/JUCE/CMakeLists.txt")
        set(JUCE_DIR "$ENV{HOME}/JUCE" CACHE PATH "Path to JUCE repository" FORCE)
    elseif(EXISTS "C:/JUCE/CMakeLists.txt")
        set(JUCE_DIR "C:/JUCE" CACHE PATH "Path to JUCE repository" FORCE)
    endif()
endif()

if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found. Please set the JUCE_DIR environment variable or CMake cache variable to your JUCE installation path.")
endif()

add_subdirectory("${JUCE_DIR}" JUCE)

# --- 3. App Definition ---
juce_add_gui_app(SampledexChordLab
    COMPANY_NAME "Sampledex"
    PRODUCT_NAME "Sampledex ChordLab"
    VERSION "2.0.0"
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "Sampledex ChordLab uses Bluetooth to discover and connect Bluetooth MIDI devices."
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sampledex ChordLab uses the microphone and audio inputs for recording audio tracks."
)

juce_generate_juce_header(SampledexChordLab)

# --- 4. Source Files ---
target_sources(SampledexChordLab PRIVATE
    # Entry Point
    Source/app/Main.cpp
    Source/app/MainComponent.h
    Source/app/MainComponent.cpp
    Source/core/AppMetadata.h
    
    # DAW Engine
    Source/engine/Track.h
    Source/ui/Mixer.h
    Source/engine/TimelineModel.h
    Source/engine/TransportEngine.h
    Source/ui/TimelineComponent.h
    Source/ui/PianoRollComponent.h
    Source/ui/StepSequencerComponent.h
    Source/ui/LcdDisplay.h
    Source/project/ProjectSerializer.h
    Source/engine/ScheduledMidiOutput.h
    Source/engine/RealtimeGraphScheduler.h
    Source/audio/StreamingClipSource.h
    Source/audio/StreamingClipSource.cpp
    
    # Utilities
    Source/core/Theme.h
    Source/audio/MidiDeviceRouter.h
    Source/audio/MidiDeviceRouter.cpp
    Source/engine/ChordEngine.h
    Source/engine/ChordEngine.cpp
)

target_include_directories(SampledexChordLab PRIVATE
    Source
    Source/app
    Source/audio
    Source/core
    Source/engine
    Source/project
    Source/ui
)

# --- 5. Flags ---
target_compile_definitions(SampledexChordLab PUBLIC
    JUCE_REPORT_APP_USAGE=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_SILENCE_XCODE_15_LINKER_WARNING=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(SampledexChordLab PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

target_compile_options(SampledexChordLab PRIVATE
    $<$<CONFIG:Release>:-O3 -funroll-loops>
)

set_target_properties(SampledexChordLab PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.Sampledex.SampledexChordLab"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.Sampledex.SampledexChordLab"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_NSBluetoothAlwaysUsageDescription "Sampledex ChordLab uses Bluetooth to discover and connect Bluetooth MIDI devices."
    XCODE_ATTRIBUTE_INFOPLIST_KEY_NSBluetoothPeripheralUsageDescription "Sampledex ChordLab can connect to Bluetooth MIDI hardware."
    XCODE_ATTRIBUTE_INFOPLIST_KEY_NSMicrophoneUsageDescription "Sampledex ChordLab uses audio inputs for microphone and line recording."
)

if(APPLE)
    add_custom_command(TARGET SampledexChordLab POST_BUILD
        COMMAND /bin/sh -c
            "PLIST=\"$<TARGET_BUNDLE_CONTENT_DIR:SampledexChordLab>/Info.plist\"; \
/usr/libexec/PlistBuddy -c \"Set :NSBluetoothAlwaysUsageDescription 'Sampledex ChordLab uses Bluetooth to discover and connect Bluetooth MIDI devices.'\" \"$PLIST\" 2>/dev/null || \
/usr/libexec/PlistBuddy -c \"Add :NSBluetoothAlwaysUsageDescription string 'Sampledex ChordLab uses Bluetooth to discover and connect Bluetooth MIDI devices.'\" \"$PLIST\"; \
/usr/libexec/PlistBuddy -c \"Set :NSBluetoothPeripheralUsageDescription 'Sampledex ChordLab can connect to Bluetooth MIDI hardware.'\" \"$PLIST\" 2>/dev/null || \
/usr/libexec/PlistBuddy -c \"Add :NSBluetoothPeripheralUsageDescription string 'Sampledex ChordLab can connect to Bluetooth MIDI hardware.'\" \"$PLIST\"; \
/usr/libexec/PlistBuddy -c \"Set :NSMicrophoneUsageDescription 'Sampledex ChordLab uses audio inputs for microphone and line recording.'\" \"$PLIST\" 2>/dev/null || \
/usr/libexec/PlistBuddy -c \"Add :NSMicrophoneUsageDescription string 'Sampledex ChordLab uses audio inputs for microphone and line recording.'\" \"$PLIST\""
        VERBATIM
    )
endif()
