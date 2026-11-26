cmake_minimum_required(VERSION 3.22)
project(MELTIT VERSION 0.8.4 LANGUAGES C CXX)

set(JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE" CACHE PATH "Local JUCE checkout")
add_subdirectory("${JUCE_SOURCE_DIR}" juce)

juce_add_plugin(MELTIT
    COMPANY_NAME "Producersources"
    BUNDLE_ID "com.producersources.meltit"
    PRODUCT_NAME "MELTIT"
    PLUGIN_MANUFACTURER_CODE Tprj
    PLUGIN_CODE Tpm2
    VST3_CATEGORIES Fx Modulation
    NEEDS_MIDI_INPUT TRUE
    FORMATS VST3 Standalone
    COPY_PLUGIN_AFTER_BUILD FALSE)

set_target_properties(MELTIT_Standalone PROPERTIES OUTPUT_NAME "MELTIT")
set(meltit_font_licenses
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/Fontshare-FFL.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/AzeretMono-OFL.txt")
if(APPLE)
    # Bundle layout: only executables belong in Contents/MacOS.
    set(meltit_standalone_license_dir "$<TARGET_FILE_DIR:MELTIT_Standalone>/../Resources")
else()
    set(meltit_standalone_license_dir "$<TARGET_FILE_DIR:MELTIT_Standalone>")
endif()
add_custom_command(TARGET MELTIT_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${meltit_standalone_license_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${meltit_font_licenses} "${meltit_standalone_license_dir}"
    VERBATIM)
add_custom_command(TARGET MELTIT_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MELTIT_VST3>/../Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${meltit_font_licenses} "$<TARGET_FILE_DIR:MELTIT_VST3>/../Resources"
    VERBATIM)

# The editor artwork is drawn in code (Source/RetroArt.cpp); only the typefaces are embedded.
juce_add_binary_data(MELTITArt HEADER_NAME CartridgeArt.h NAMESPACE CartridgeArt
    SOURCES
        Assets/Fonts/TechnorSemibold.otf
        Assets/Fonts/PilcrowRoundedMedium.otf
        Assets/Fonts/PilcrowRoundedSemibold.otf
        Assets/Fonts/AzeretMonoRegular.otf)

# The preset library ships inside the plug-in as one zip and is unpacked into the user's data folder on first run.
file(GLOB_RECURSE meltit_preset_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Presets/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${meltit_preset_files})
set(meltit_preset_zip "${CMAKE_CURRENT_BINARY_DIR}/MeltitPresets.zip")
file(REMOVE "${meltit_preset_zip}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar cf "${meltit_preset_zip}" --format=zip Presets
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Resources"
    RESULT_VARIABLE meltit_zip_result)
if(NOT meltit_zip_result EQUAL 0)
    message(FATAL_ERROR "Could not pack Resources/Presets")
endif()
juce_add_binary_data(MELTITPresets HEADER_NAME MeltitPresets.h NAMESPACE MeltitPresets
    SOURCES "${meltit_preset_zip}")

set(plugin_sources
    Source/RackProcessor.cpp
    Source/RackEditor.cpp
    Source/RackEditor.h
    Source/RetroArt.cpp
    Source/RetroArt.h
    Source/RackPresets.h
    Source/PresetLibrary.h
    Source/PresetFile.h
    Source/Dsp/Effects.h
    Source/Dsp/GlobalEffects.h
    Source/Dsp/Modulation.h
    Source/Dsp/ReverseReverb.h
    Source/Dsp/SpatialEffects.h
    Source/Dsp/TapeDsp.h
    Source/Dsp/TapeTexture.h
    Source/Dsp/TimeStretch.h
    Source/Dsp/VhsDegrade.h
    Source/Dsp/TapeTransport.h
    Source/Dsp/GenerationLoss.h)
target_sources(MELTIT PRIVATE ${plugin_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" PREFIX Source FILES ${plugin_sources})
target_include_directories(MELTIT PRIVATE Source/Dsp)
target_include_directories(MELTIT SYSTEM PRIVATE ThirdParty/Signalsmith)
target_compile_features(MELTIT PRIVATE cxx_std_20)
target_compile_options(MELTIT PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_compile_definitions(MELTIT PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(MELTIT PRIVATE MELTITArt MELTITPresets juce::juce_audio_processors juce::juce_audio_utils juce::juce_dsp)

option(BUILD_TESTING "Build JUCE editor and transport tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(Tests)
endif()
