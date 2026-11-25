add_executable(TapeMultiFxTransportTest test_transport.cpp)
add_executable(TapeMultiFxEditorTest test_editor.cpp)

foreach(test_target IN ITEMS TapeMultiFxTransportTest TapeMultiFxEditorTest)
    target_compile_features(${test_target} PRIVATE cxx_std_20)
    target_include_directories(${test_target} PRIVATE
        "${PROJECT_SOURCE_DIR}/Source"
        "${PROJECT_SOURCE_DIR}/Source/Dsp"
        "${JUCE_SOURCE_DIR}/modules")
    target_compile_definitions(${test_target} PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(${test_target} PRIVATE TapeMultiFx)
endforeach()

add_test(NAME TapeMultiFx.Transport COMMAND TapeMultiFxTransportTest)
add_test(NAME TapeMultiFx.Editor COMMAND TapeMultiFxEditorTest
    "${CMAKE_CURRENT_BINARY_DIR}/editor-preview.png"
    "${PROJECT_SOURCE_DIR}/Resources/Presets/3. BASIC FX/REVERSE/-OCTAVE 1-2.vstpreset"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-chorus.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-wide.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-four-effects.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-saturation.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-saturation-125.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-compact-consistency.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-plus-przewijanie.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-stretch.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-stretch-map.png"
    "${CMAKE_CURRENT_BINARY_DIR}/electric-frame.png"
    "${CMAKE_CURRENT_BINARY_DIR}/knob-drop.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-vhs.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-cables.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-cables-scrolled.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-mod-drawer.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-mod-lfo.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-grain.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-grain-controls.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-mod-xy.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-cables-docked.png"
    "${CMAKE_CURRENT_BINARY_DIR}/editor-cable-feedthrough.png")
