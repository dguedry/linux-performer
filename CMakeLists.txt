cmake_minimum_required(VERSION 3.22)
# Version: CI passes the tag (v1.2.3 -> 1.2.3); local builds use the default.
set(PERFORMER_VERSION "0.1.29" CACHE STRING "Performer version")
project(Performer VERSION ${PERFORMER_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)   # keeps JUCE's own install rules out of ours

juce_add_gui_app(Performer
    PRODUCT_NAME "Performer"
    COMPANY_NAME "dguedry"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/performer-512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/performer-64.png"
    VERSION ${PROJECT_VERSION})

# App icon, plus the user manual: embedding docs/MANUAL.md rather than copying it
# means the in-app help cannot drift from the file in the repository.
juce_add_binary_data(PerformerAssets
    SOURCES Resources/performer-512.png Resources/performer-256.png Resources/performer-64.png
            docs/MANUAL.md)

target_sources(Performer PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/PluginHost.cpp
    Source/PluginScanner.cpp
    Source/PluginIcons.cpp
    Source/MappingSuggestions.cpp
    Source/Ipc/Protocol.cpp
    Source/RemotePlugin.cpp
    Source/LowLatency.cpp
    Source/HelpWindow.cpp
    Source/RemoteServer.cpp Source/Hotspot.cpp Source/QrCode.cpp Source/PluginView.cpp
    Source/Engine.cpp
    Source/MainComponent.cpp)

target_include_directories(Performer PRIVATE Source)

target_compile_definitions(Performer PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Performer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Performer,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ALSA=1
    JUCE_JACK=1
    JUCE_JACK_CLIENT_NAME="Performer"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(Performer PRIVATE
    PerformerAssets
    ${CMAKE_DL_LIBS}
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# performer-plugin-host: one plugin per process
juce_add_gui_app(PerformerPluginHost
    PRODUCT_NAME "performer-plugin-host"
    COMPANY_NAME "dguedry"
    VERSION ${PROJECT_VERSION})

target_sources(PerformerPluginHost PRIVATE
    Source/PluginHostProcess/PluginHostMain.cpp
    Source/Ipc/Protocol.cpp)

target_include_directories(PerformerPluginHost PRIVATE Source
    external/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK)

target_compile_definitions(PerformerPluginHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(PerformerPluginHost PRIVATE
    PerformerAssets
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

add_dependencies(Performer PerformerPluginHost)

# Put the helper next to the main executable so it is found at runtime.
add_custom_command(TARGET Performer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:PerformerPluginHost> $<TARGET_FILE_DIR:Performer>/performer-plugin-host
    COMMENT "Copying performer-plugin-host next to Performer")

#------------------------------------------------------------------------------
# Tests
enable_testing()
juce_add_console_app(ModelTest PRODUCT_NAME "ModelTest")
target_sources(ModelTest PRIVATE Tests/ModelTest.cpp Source/Model.cpp Source/MappingSuggestions.cpp Source/QrCode.cpp)
target_include_directories(ModelTest PRIVATE Source)
target_compile_definitions(ModelTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_VST=0)
target_link_libraries(ModelTest PRIVATE
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_audio_processors
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME ModelTest COMMAND ModelTest)

juce_add_console_app(RemoteTest PRODUCT_NAME "RemoteTest")
target_sources(RemoteTest PRIVATE Tests/RemoteTest.cpp Source/RemoteServer.cpp Source/Hotspot.cpp Source/QrCode.cpp Source/PluginView.cpp Source/PluginView.cpp Source/QrCode.cpp Source/Model.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/PluginIcons.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
target_include_directories(RemoteTest PRIVATE Source)
target_compile_definitions(RemoteTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_JACK=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1 JUCE_PLUGINHOST_LADSPA=1 JUCE_PLUGINHOST_VST=0)
target_link_libraries(RemoteTest PRIVATE
    PerformerAssets
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_graphics juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME RemoteTest COMMAND RemoteTest)

juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
target_sources(EngineTest PRIVATE Tests/EngineTest.cpp Source/QrCode.cpp Source/Model.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/PluginIcons.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
target_include_directories(EngineTest PRIVATE Source)
target_compile_definitions(EngineTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_JACK=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1 JUCE_PLUGINHOST_LADSPA=1 JUCE_PLUGINHOST_VST=0)
target_link_libraries(EngineTest PRIVATE
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_graphics juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(EngineTest PerformerPluginHost)
add_test(NAME EngineTest COMMAND EngineTest)
set_tests_properties(EngineTest PROPERTIES ENVIRONMENT "PERFORMER_PLUGIN_HOST=$<TARGET_FILE:PerformerPluginHost>")

#------------------------------------------------------------------------------
# Install: `cmake --install build --prefix ~/.local` (or /usr/local) puts the two
# binaries side by side (Performer finds its helper next to itself), plus the
# launcher entry and icon.
install(TARGETS Performer PerformerPluginHost RUNTIME DESTINATION bin)
install(FILES Resources/performer.desktop DESTINATION share/applications)
install(FILES Resources/performer.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(size 16 24 32 48 64 128 256 512)
    install(FILES Resources/performer-${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME performer.png)
endforeach()

#------------------------------------------------------------------------------
# Packages: `cpack -G DEB` / `cpack -G RPM` in the build dir (the GitHub workflow
# does both and attaches them to releases). Linked libraries are picked up
# automatically; the X11/GL libraries JUCE opens at runtime with dlopen are listed
# by hand.
set(CPACK_PACKAGE_NAME "performer")
set(CPACK_PACKAGE_VENDOR "dguedry")
set(CPACK_PACKAGE_CONTACT "Donnie Guedry <dguedry@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/dguedry/linux-performer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Live-performance plugin host for Linux")
set(CPACK_PACKAGE_DESCRIPTION "Performer is a live-performance MIDI-to-plugin host: 128 programs per keyboard selected by MIDI Program Change, VST3/LV2/LADSPA instruments and effects with splits and velocity layers, MIDI learn, every plugin in its own process, low-latency PipeWire/JACK audio.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_FILE_NAME "performer-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1, libgl1")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire-jack | libjack0 | libjack-jackd2-0, pipewire-alsa | pulseaudio-utils")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "calf-plugins, yabridge")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_REQUIRES "libX11, libXext, libXrandr, libXinerama, libXcursor, mesa-libGL")
set(CPACK_RPM_PACKAGE_SUGGESTS "pipewire-jack-audio-connection-kit, calf")
# the icon and applications dirs belong to the filesystem/hicolor packages
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
    "/usr/share/icons/hicolor/scalable" "/usr/share/icons/hicolor/scalable/apps")
foreach(size 16 24 32 48 64 128 256 512)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
         "/usr/share/icons/hicolor/${size}x${size}" "/usr/share/icons/hicolor/${size}x${size}/apps")
endforeach()

include(CPack)
