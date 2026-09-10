cmake_minimum_required(VERSION 3.22)
project(Performer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE)

juce_add_gui_app(Performer
    PRODUCT_NAME "Performer"
    COMPANY_NAME "dguedry"
    VERSION ${PROJECT_VERSION})

target_sources(Performer PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/PluginHost.cpp
    Source/PluginScanner.cpp
    Source/MappingSuggestions.cpp
    Source/Ipc/Protocol.cpp
    Source/RemotePlugin.cpp
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
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(Performer PRIVATE
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

target_include_directories(PerformerPluginHost PRIVATE Source)

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
target_sources(EngineTest PRIVATE Tests/EngineTest.cpp Source/Model.cpp Source/PluginHost.cpp Source/PluginScanner.cpp Source/MappingSuggestions.cpp Source/Ipc/Protocol.cpp Source/RemotePlugin.cpp Source/Engine.cpp)
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


