cmake_minimum_required(VERSION 3.22)
project(TapeMultiFx VERSION 0.8.4 LANGUAGES C CXX)

set(JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/JUCE" CACHE PATH "Local JUCE checkout")
add_subdirectory("${JUCE_SOURCE_DIR}" juce)

juce_add_plugin(TapeMultiFx
    COMPANY_NAME "Producersources"
    BUNDLE_ID "com.producersources.meltit"
    PRODUCT_NAME "Tape Multi-FX"
    PLUGIN_MANUFACTURER_CODE Tprj
    PLUGIN_CODE Tpm2
    VST3_CATEGORIES Fx Modulation
    NEEDS_MIDI_INPUT TRUE
    FORMATS VST3 Standalone
    COPY_PLUGIN_AFTER_BUILD FALSE)

set_target_properties(TapeMultiFx_Standalone PROPERTIES OUTPUT_NAME "MELTIT")
if(APPLE)
    # JUCE writes CFBundleExecutable from the shared PRODUCT_NAME ("Tape Multi-FX"); it must match
    # the renamed executable or macOS reports the .app as "damaged or incomplete".
    add_custom_command(TARGET TapeMultiFx_Standalone POST_BUILD
        COMMAND /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable MELTIT"
            "$<TARGET_BUNDLE_CONTENT_DIR:TapeMultiFx_Standalone>/Info.plist"
        VERBATIM)
endif()
set(meltit_font_licenses
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/Fontshare-FFL.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts/AzeretMono-OFL.txt")
if(APPLE)
    # Bundle layout: only executables belong in Contents/MacOS.
    set(meltit_standalone_license_dir "$<TARGET_FILE_DIR:TapeMultiFx_Standalone>/../Resources")
else()
    set(meltit_standalone_license_dir "$<TARGET_FILE_DIR:TapeMultiFx_Standalone>")
endif()
add_custom_command(TARGET TapeMultiFx_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${meltit_standalone_license_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${meltit_font_licenses} "${meltit_standalone_license_dir}"
    VERBATIM)
add_custom_command(TARGET TapeMultiFx_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TapeMultiFx_VST3>/../Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${meltit_font_licenses} "$<TARGET_FILE_DIR:TapeMultiFx_VST3>/../Resources"
    VERBATIM)

# The editor artwork is drawn in code (Source/RetroArt.cpp); only the typefaces are embedded.
juce_add_binary_data(TapeMultiFxArt HEADER_NAME CartridgeArt.h NAMESPACE CartridgeArt
    SOURCES
        Assets/Fonts/TechnorSemibold.otf
        Assets/Fonts/PilcrowRoundedMedium.otf
        Assets/Fonts/PilcrowRoundedSemibold.otf
        Assets/Fonts/AzeretMonoRegular.otf)

set(plugin_sources
    Source/RackProcessor.cpp
    Source/RackEditor.cpp
    Source/RackEditor.h
    Source/RetroArt.cpp
    Source/RetroArt.h
    Source/RackPresets.h
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
target_sources(TapeMultiFx PRIVATE ${plugin_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" PREFIX Source FILES ${plugin_sources})
target_include_directories(TapeMultiFx PRIVATE Source/Dsp)
target_include_directories(TapeMultiFx SYSTEM PRIVATE ThirdParty/Signalsmith)
target_compile_features(TapeMultiFx PRIVATE cxx_std_20)
target_compile_options(TapeMultiFx PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_compile_definitions(TapeMultiFx PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(TapeMultiFx PRIVATE TapeMultiFxArt juce::juce_audio_processors juce::juce_audio_utils juce::juce_dsp)

option(BUILD_TESTING "Build JUCE editor and transport tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(Tests)
endif()
