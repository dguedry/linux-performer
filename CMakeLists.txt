cmake_minimum_required(VERSION 3.22)
project(Performer VERSION 0.1.0 LANGUAGES C CXX)

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

# App icon, embedded so the window (and the helper's editor windows) can set it.
juce_add_binary_data(PerformerAssets
    SOURCES Resources/performer-256.png Resources/performer-64.png)

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
target_sources(ModelTest PRIVATE Tests/ModelTest.cpp Source/Model.cpp Source/MappingSuggestions.cpp)
target_include_directories(ModelTest PRIVATE Source)
target_compile_definitions(ModelTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_VST=0)
target_link_libraries(ModelTest PRIVATE
    juce::juce_core juce::juce_data_structures juce::juce_events
    juce::juce_audio_processors
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_test(NAME ModelTest COMMAND ModelTest)

juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
target_sources(EngineTest PRIVATE Tests/EngineTest.cpp Source/Model.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/PluginIcons.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
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
