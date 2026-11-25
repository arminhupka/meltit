#pragma once

#include "RackPresets.h"
#include "Modulation.h"
#include "Granular.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

namespace rack
{
inline constexpr size_t editorModuleCount = 11;
inline constexpr int cartridgeSlots = 12;
inline constexpr int cartridgeCopies = 2;
inline constexpr int noCartridge = -2;
inline constexpr int emptyCartridge = -1;
using CartridgeOrder = std::array<int, cartridgeSlots>;

struct DelayPreview
{
    float milliseconds, feedback, mix;
    bool active;
    bool operator==(const DelayPreview&) const = default;
};

// The Stretch program as the map display draws it, plus where the audio thread currently is in it.
struct StretchPreview
{
    float ratio = 1, start = 0, land = 0, phase = 0, debtSeconds = 0, current = 1;
    int shape = 0;
    bool active = false, stretching = false;
    bool operator==(const StretchPreview&) const = default;
};

class EditorModel
{
public:
    virtual ~EditorModel() = default;
    virtual DelayPreview delayPreview(int instance) const = 0;
    virtual StretchPreview stretchPreview(int instance) const = 0;
    // Where the grains of a Grain card are now, for its screen.
    virtual GrainPicture grainPicture(int instance) const = 0;
    virtual std::array<float, 2> takeOutputPeaks() = 0;
    virtual void loadPreset(int index) = 0;
    virtual bool loadPresetFile(const juce::File& file) = 0;
    virtual void generate(int tag, uint32_t seed) = 0;
    virtual PresetIdentity presetIdentity() const = 0;
    virtual CartridgeOrder cartridges() const = 0;
    virtual bool insertCartridge(int type, int position) = 0;
    virtual void ejectCartridge(int position) = 0;
    // A cartridge is on its way out (or, with false, stays after all): its effect fades to silence meanwhile.
    virtual void fadeOutCartridge(int instance, bool leaving) = 0;
    virtual void moveCartridge(int source, int destination) = 0;
    virtual void clearCartridges() = 0;
    virtual void toggleCartridge(int position) = 0;
    // A hand on the tape reels: while `engaged` the tape follows `speed` (1 normal, 0 held, below 0 backwards);
    // letting go hands it back to the motor.
    virtual void scrubTape(bool engaged, float speed) = 0;
    // How fast the tape runs now: 1 normal, 0 stopped, negative while pulled backwards.
    virtual float tapeSpeed() const = 0;
    // The window size the user chose, as a multiple of the design size; kept across closing and reopening the editor.
    virtual float editorScale() const = 0;
    virtual void setEditorScale(float scale) = 0;
    // What the modulation sources are doing now, for the patch bay's lamps and the MOD drawer's screens.
    virtual ModulationActivity modulationActivity() const = 0;
};

juce::AudioProcessorEditor* makeCartridgeEditor(juce::AudioProcessor&, juce::AudioProcessorValueTreeState&, EditorModel&);
}
