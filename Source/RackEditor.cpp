#include "RackEditor.h"
#include "RackPresets.h"
#include "RetroArt.h"
#include "Modulation.h"
#include "TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rack
{
namespace
{
const juce::Colour panel { 0xff0d1317 };
const juce::Colour border { 0xff3b4650 };
const juce::Colour ink { 0xfff2f5f6 };
const juce::Colour muted { 0xffaab9c4 };
const juce::Colour accent { 0xffffa45b };
const juce::Colour metalInk = retro::palette::metalInk;

using retro::font;
using retro::headingFont;
using retro::readoutFont;

// Keeps artwork that only changes with its size as an image at the screen's pixel density. The procedural
// finishes are costly to draw, and without this every small repaint (a knob turning, the meters ticking)
// would redraw the whole panel or window behind it.
class PaintCache
{
public:
    template <typename Painter>
    void draw(juce::Graphics& g, juce::Rectangle<int> bounds, Painter&& painter)
    {
        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        if (!image.isValid() || bounds != area || std::abs(scale - pixelScale) > .001f)
        {
            area = bounds;
            pixelScale = scale;
            image = juce::Image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt(static_cast<float>(bounds.getWidth()) * scale)),
                                juce::jmax(1, juce::roundToInt(static_cast<float>(bounds.getHeight()) * scale)), true);
            juce::Graphics painted(image);
            painted.addTransform(juce::AffineTransform::translation(static_cast<float>(-bounds.getX()),
                                                                    static_cast<float>(-bounds.getY()))
                                     .scaled(scale));
            painter(painted);
        }
        g.drawImageTransformed(image, juce::AffineTransform::scale(1.f / pixelScale)
                                          .translated(static_cast<float>(area.getX()), static_cast<float>(area.getY())));
    }

    void clear() { image = {}; }

private:
    juce::Image image;
    juce::Rectangle<int> area;
    float pixelScale = 1.f;
};

void label(juce::Label& control, float size, juce::Colour colour, bool bold = false)
{
    control.setFont(font(size, bold));
    control.setColour(juce::Label::textColourId, colour);
    control.setBorderSize({});
}

enum class ButtonFace
{
    selector, toggle, lamp, previous, next, options, controls, close, generate, presetName,
    inspectorToggle, inspectorClose
};

void styleButton(juce::TextButton& control, ButtonFace face)
{
    control.getProperties().set("face", static_cast<int>(face));
    control.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    // Clicking keeps the hardware look; the focus ring is only for keyboard navigation.
    control.setMouseClickGrabsKeyboardFocus(false);
}

struct ModuleInfo
{
    const char* name;
    const char* prefix;
};

constexpr std::array<ModuleInfo, editorModuleCount> modules {{
    { "Tape", "tape" }, { "Tremolo", "tremolo" }, { "Saturation", "saturation" },
    { "Resampler", "resampler" }, { "Chorus", "chorus" }, { "Delay", "delay" },
    { "Reverse", "reverse" }, { "Reverb", "reverb" }, { "Stretch", "stretch" }, { "VHS", "vhs" },
    { "Grain", "grain" }
}};

// Overshoots its end a little and settles back, like a part landing against a spring stop.
float easeOutBack(float u)
{
    constexpr float overshoot = 1.70158f;
    const auto v = u - 1.f;
    return 1.f + (overshoot + 1.f) * v * v * v + overshoot * v * v;
}

// Where a rotary knob sits inside its slider bounds and how large it is.
juce::Rectangle<float> knobArea(const juce::Slider& slider, juce::Rectangle<int> sliderBounds)
{
    const auto& properties = slider.getProperties();
    const auto compact = static_cast<bool>(properties["cassette_compact"]);
    const auto hero = static_cast<bool>(properties["cassette_hero"]);
    const auto room = juce::jmin(sliderBounds.getWidth(), sliderBounds.getHeight());
    // A hero knob leaves room around it for its printed scale.
    const auto diameter = hero ? juce::jmin(room - 10, 70) : juce::jmin(room, compact ? 47 : 52);
    return juce::Rectangle<float>(static_cast<float>(diameter), static_cast<float>(diameter))
        .withCentre(sliderBounds.toFloat().getCentre());
}

// How high a knob hovers above its panel while it drops into place: 0 when it rests on the panel.
float knobDrop(const juce::Component& slider)
{
    return static_cast<float>(slider.getProperties().getWithDefault("cassette_drop", 0.f));
}

// A knob's shadow on the panel. Seen from above, a knob held higher throws its shadow further away, larger and
// fainter; it tightens back under the knob as the knob lands.
void droppedKnobShadow(juce::Graphics& g, juce::Point<float> centre, float diameter, float height)
{
    if (height <= .001f)
    {
        retro::knobShadow(g, centre, diameter);
        return;
    }
    g.beginTransparencyLayer(1.f - .7f * height);
    retro::knobShadow(g, centre.translated(9.f * height, 24.f * height), diameter * (1.f + .6f * height));
    g.endTransparencyLayer();
}

// Height through a drop, `u` running from 0 (released) to 1 (at rest): it falls under gravity, strikes the
// panel and hops once.
float dropHeight(float u)
{
    u = juce::jlimit(0.f, 1.f, u);
    constexpr float impact = .72f;
    if (u < impact)
    {
        const auto v = u / impact;
        return 1.f - v * v;
    }
    return .1f * std::sin(juce::MathConstants<float>::pi * (u - impact) / (1.f - impact));
}

// Parameter ids of the knobs each modulation target drives, in the order of rack::ModTarget.
constexpr std::array<const char*, modTargetCount> modulationKnobs {
    "", "tape_depth", "tape_wow_rate", "tremolo_depth", "tremolo_rate", "saturation_drive", "saturation_mix",
    "resampler_drive", "resampler_rate", "chorus_depth", "chorus_saturation", "chorus_mix", "delay_feedback",
    "delay_saturation", "delay_mix", "reverse_forward", "reverse_pitch", "reverse_mix", "reverb_decay",
    "reverb_mix", "stretch_ratio", "stretch_cycle", "stretch_mix", "vhs_wear", "vhs_tracking", "vhs_wobble",
    "vhs_mix", "tape_flutter_rate", "tape_flutter_amount", "tremolo_shape", "saturation_frequency",
    "saturation_tone", "resampler_jitter", "chorus_rate", "chorus_width", "delay_analog", "reverse_highpass",
    "reverb_size", "reverb_pre_delay", "grain_size", "grain_density", "grain_pitch", "grain_mix",
    "grain_position", "grain_feedback", "grain_window", "grain_pitch_random", "grain_size_random",
    "grain_density_random", "grain_width", "grain_offset", "grain_voices", "grain_stretch_speed",
    "grain_tone_lowpass", "grain_tone_highpass" };

// The modulation sources in the order of their jacks: parameter prefix and cable colour.
constexpr std::array<const char*, 7> sourcePrefixes { "lfo_1_", "lfo_2_", "macro_", "env_", "seq_", "xy_x_", "xy_y_" };
constexpr std::array<juce::uint32, 7> sourceColours { 0xffe0473c, 0xfff0b030, 0xff3c98e6, 0xff4fd18b, 0xffb07cf0,
                                                      0xffe6c15a, 0xff8ff5c4 };

// A patch socket set into a panel: a chrome nut round a dark hole, larger than a knob's input.
void paintPanelSocket(juce::Graphics& g, juce::Point<float> centre, float size, float opacity, bool hot)
{
    const auto nut = juce::Rectangle<float>(size, size).withCentre(centre);
    g.setColour(juce::Colours::black.withAlpha(.5f * opacity));
    g.fillEllipse(nut.translated(.8f, 1.4f).expanded(1.f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffe4e7ea).withMultipliedAlpha(opacity), nut.getX(), nut.getY(),
                                           juce::Colour(0xff4d5156).withMultipliedAlpha(opacity), nut.getRight(), nut.getBottom(), false));
    g.fillEllipse(nut);
    g.setColour(juce::Colours::black.withAlpha(.7f * opacity));
    g.drawEllipse(nut, .8f);
    g.setColour(juce::Colour(0xff050607).withMultipliedAlpha(opacity));
    g.fillEllipse(nut.reduced(size * .28f));
    if (hot)
    {
        g.setColour(juce::Colours::white.withAlpha(.35f * opacity));
        g.drawEllipse(nut.expanded(2.f), 1.f);
    }
}

// Whether a patch cable can drive the knob with this parameter id (either copy of an effect).
bool isModulationKnob(const juce::String& id)
{
    const auto local = id.startsWith("cart2_") ? id.substring(6) : id;
    return std::any_of(modulationKnobs.begin() + 1, modulationKnobs.end(), [&local](const char* knob) { return local == knob; });
}

// Where a knob's patch input sits: a small socket beside its right edge, just above the middle, clear of the name
// above, the readout below and a large knob's printed scale.
juce::Point<float> inputSocket(const juce::Slider& slider, juce::Rectangle<float> knob)
{
    const auto hero = static_cast<bool>(slider.getProperties()["cassette_hero"]);
    return knob.getCentre() + juce::Point<float>(.94f, -.34f) * (knob.getWidth() * .5f + (hero ? 11.f : 8.f));
}

// A knob's patch input: a chrome ring around a dark hole. `glow` (0 to 1) lights it in `colour` while a cable is
// being carried, to show where it can go.
void paintInputSocket(juce::Graphics& g, juce::Point<float> centre, float glow = 0, juce::Colour colour = {})
{
    if (glow > 0)
    {
        g.setColour(colour.withAlpha(.25f * glow));
        g.fillEllipse(juce::Rectangle<float>(16, 16).withCentre(centre));
        g.setColour(colour.withAlpha(.9f * glow));
        g.drawEllipse(juce::Rectangle<float>(12, 12).withCentre(centre), 1.5f);
    }
    const auto ring = juce::Rectangle<float>(8, 8).withCentre(centre);
    g.setColour(juce::Colours::black.withAlpha(.45f));
    g.fillEllipse(ring.translated(.6f, 1.f).expanded(.6f));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xffe4e7ea), ring.getX(), ring.getY(),
                                           juce::Colour(0xff5a5e63), ring.getRight(), ring.getBottom(), false));
    g.fillEllipse(ring);
    g.setColour(juce::Colour(0xff050607));
    g.fillEllipse(ring.reduced(2.f));
}

class ParameterTile final : public juce::Component
{
public:
    ParameterTile(juce::AudioProcessorValueTreeState& state, juce::RangedAudioParameter& parameter,
                  const juce::String& shortName, size_t effect, juce::Colour captionColour, bool compact)
        : compactTile(compact)
    {
        const auto id = parameter.paramID;
        name.setText(shortName, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        label(name, compact ? 15.f : 14.5f, captionColour, true);
        addAndMakeVisible(name);
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&parameter))
        {
            for (int i = 0; i < choice->choices.size(); ++i)
                combo.addItem(choice->choices[i], i + 1);
            combo.setTitle(parameter.getName(100));
            combo.setComponentID(id);
            combo.setJustificationType(juce::Justification::centred);
            combo.setMouseClickGrabsKeyboardFocus(false);
            addAndMakeVisible(combo);
            comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(state, id, combo);
        }
        else if (dynamic_cast<juce::AudioParameterBool*>(&parameter))
        {
            styleButton(toggle, compact ? ButtonFace::toggle : ButtonFace::inspectorToggle);
            const auto stereo = !compact && id.endsWith("_stereo");
            toggle.getProperties().set("stereo_switch", stereo);
            toggle.setClickingTogglesState(true);
            toggle.setTitle(parameter.getName(100));
            toggle.setComponentID(id);
            toggle.onStateChange = [this, stereo]
            {
                const auto enabled = toggle.getToggleState();
                toggle.setButtonText(stereo ? (enabled ? "Stereo" : "Mono") : (enabled ? "ON" : "OFF"));
            };
            addAndMakeVisible(toggle);
            buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, id, toggle);
            toggle.onStateChange();
        }
        else
        {
            slider.getProperties().set("cassette_effect", static_cast<int>(effect));
            slider.getProperties().set("cassette_compact", compact);
            slider.getProperties().set("cassette_ink", static_cast<juce::int64>(captionColour.getARGB()));
            slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            // A knob held up during its drop is drawn larger than its bounds; the panels repaint around it.
            slider.setPaintingIsUnclipped(true);
            setPaintingIsUnclipped(true);
            // The wheel scrolls the rack and panels instead of turning whichever knob is under the pointer.
            slider.setScrollWheelEnabled(false);
            slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 22);
            slider.setTitle(parameter.getName(100));
            slider.setComponentID(id);
            addAndMakeVisible(slider);
            sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state, id, slider);
            // Keep the original VST3 parameter range for existing playback automation and presets.
            if (id == "delay_feedback")
                slider.setRange(slider.getMinimum(), 100.0, slider.getInterval());
            slider.setTextValueSuffix(parameter.getLabel().isNotEmpty() ? " " + parameter.getLabel() : juce::String{});
        }
    }

    // Progress of the power-on self test, 0 to 1; a negative value ends it.
    void setSweep(float progress)
    {
        if (sliderAttachment == nullptr)
            return;
        if (progress < 0)
            slider.getProperties().remove("cassette_sweep");
        else
            slider.getProperties().set("cassette_sweep", progress);
        slider.repaint();
    }

    // Lifts the knob `height` above the panel (0 rests on it, 1 is the top of its drop).
    void setDrop(float height)
    {
        if (sliderAttachment == nullptr || knobDrop(slider) == height)
            return;
        slider.getProperties().set("cassette_drop", height);
        slider.repaint();
        repaint();
    }

    float drop() const { return sliderAttachment != nullptr ? knobDrop(slider) : 0.f; }

    // The main control of a card: a larger knob with a printed scale and a larger caption.
    void setHero(bool hero)
    {
        if (static_cast<bool>(slider.getProperties()["cassette_hero"]) == hero)
            return;
        slider.getProperties().set("cassette_hero", hero);
        name.setFont(font(hero ? 16.5f : 15.f, true));
        slider.resized();
        slider.repaint();
    }

    // Where this tile's knob sits, in tile coordinates; empty for switches and selectors.
    juce::Rectangle<float> knob()
    {
        if (sliderAttachment == nullptr || !slider.isVisible())
            return {};
        const auto layout = slider.getLookAndFeel().getSliderLayout(slider);
        const auto area = knobArea(slider, layout.sliderBounds).translated(static_cast<float>(slider.getX()),
                                                                          static_cast<float>(slider.getY()));
        return area.getWidth() >= 12 ? area : juce::Rectangle<float>();
    }

    // A narrow tile would clip its knob's long shadow, so the panel holding it can paint the shadow instead.
    void leaveShadowToParent() { ownShadow = false; }

    // The knob's long shadow falls outside the slider, so the tile paints it onto the panel beneath.
    void paint(juce::Graphics& g) override
    {
        const auto area = knob();
        if (ownShadow && !area.isEmpty())
            droppedKnobShadow(g, area.getCentre(), area.getWidth(), drop());
        // Knobs that a patch cable can drive carry a small input socket, on the cards and in the controls window.
        if (!area.isEmpty() && drop() <= 0.f && isModulationKnob(slider.getComponentID()))
            paintInputSocket(g, inputSocket(slider, area));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(7, 1);
        if (comboAttachment && !compactTile)
        {
            // A full-size selector hangs just under its caption, the pair centred in the tile, so the caption can
            // never run into it and both share one centre line.
            const auto width = juce::jmin(140, area.getWidth());
            auto group = area.withSizeKeepingCentre(width, 16 + 8 + 28);
            name.setBounds(group.removeFromTop(16));
            combo.setBounds(group.withTrimmedTop(8));
            return;
        }
        name.setBounds(area.removeFromTop(16));
        if (sliderAttachment)
            slider.setBounds(area.withTrimmedTop(2));
        if (comboAttachment)
            combo.setBounds(area.withSizeKeepingCentre(juce::jmin(140, area.getWidth()), 28)
                               .translated(0, compactTile ? -9 : -13));
        if (buttonAttachment)
        {
            const auto width = juce::jmin(compactTile ? 60 : 116, area.getWidth());
            toggle.setBounds(area.withSizeKeepingCentre(width, compactTile ? 24 : 32)
                                 .translated(0, compactTile ? 0 : -13));
        }
    }

private:
    const bool compactTile;
    bool ownShadow = true;
    juce::Label name;
    juce::Slider slider;
    juce::ComboBox combo;
    juce::TextButton toggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
};

class ModuleButton : public juce::TextButton
{
public:
    std::function<void(const juce::MouseEvent&)> onDrag, onDrop;

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragging = false;
        juce::TextButton::mouseDown(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!dragging && (!event.mods.isLeftButtonDown() || event.getDistanceFromDragStart() < 6))
        {
            juce::TextButton::mouseDrag(event);
            return;
        }
        if (!dragging)
        {
            dragging = true;
            setState(buttonNormal);
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        }
        if (onDrag)
            onDrag(event);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!dragging)
        {
            juce::TextButton::mouseUp(event);
            return;
        }
        dragging = false;
        setState(buttonNormal);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        if (onDrop)
            onDrop(event);
    }

private:
    bool dragging = false;
};

class EchoEnvelope final : public juce::Component
{
public:
    void update(DelayPreview preview)
    {
        if (data != preview)
        {
            data = preview;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced(.5f);
        g.setColour(panel);
        g.fillRoundedRectangle(area, 6);
        g.setColour(border);
        g.drawRoundedRectangle(area, 6, 1);
        area.reduce(12, 7);
        auto heading = area.removeFromTop(20);
        g.setColour(muted);
        g.setFont(font(13, true));
        g.drawText("Echo envelope", heading, juce::Justification::centredLeft);
        g.setFont(readoutFont(13.f));
        g.drawText(juce::String(juce::roundToInt(data.milliseconds)) + " ms / repeat",
                   heading, juce::Justification::centredRight);
        area.removeFromTop(4);

        // A real time axis: the window steps through 1-2-5 lengths so it always holds at least four repeats, and
        // the repeats sit at n x Time, so turning Time slides them along until the window rescales.
        constexpr float floorDb = -48.f;
        const auto ms = juce::jlimit(.5f, 15900.f, data.milliseconds);
        auto span = 1.f;
        int divisions = 10;
        for (auto decade = 1.f; span < 4 * ms; decade *= 10)
            for (const auto& [factor, parts] : { std::pair { 1.f, 10 }, std::pair { 2.f, 4 }, std::pair { 5.f, 5 } })
                if (span < 4 * ms)
                    span = factor * decade, divisions = parts;
        const auto width = juce::jlimit(3.f, 8.f, area.getWidth() / 90);
        const auto first = area.getX() + width * .5f;
        const auto pixelsPerMs = (area.getRight() - width * .5f - first) / span;
        const auto bottom = area.getBottom();
        const auto label = [](float t)
        {
            return t >= 1000 ? juce::String(t / 1000, std::fmod(t, 1000.f) == 0 ? 0 : 1) + "s"
                             : juce::String(t, t < 10 && std::fmod(t, 1.f) != 0 ? 1 : 0) + "ms";
        };
        g.setFont(readoutFont(10.f));
        for (int i = 0; i <= divisions; ++i)
        {
            const auto t = span * i / divisions;
            const auto x = first + pixelsPerMs * t;
            g.setColour(border.withAlpha(.55f));
            g.drawVerticalLine(juce::roundToInt(x), area.getY(), bottom);
            if (i > 0)
            {
                g.setColour(muted.withAlpha(.5f));
                g.drawText(label(t), juce::Rectangle<float>(x - 44, area.getY(), 41, 11),
                           juce::Justification::centredRight);
            }
        }
        for (const auto db : { -12.f, -24.f, -36.f })
            g.drawHorizontalLine(juce::roundToInt(bottom - area.getHeight() * (1 - db / floorDb)),
                                 area.getX(), area.getRight());
        g.drawHorizontalLine(juce::roundToInt(bottom - 1), area.getX(), area.getRight());

        // Levels in dB above a -48 dB floor, so a soft feedback still shows its tail. Mix is a crossfade, so the
        // dry hit drops as the echoes rise.
        const auto heightFor = [&](float gain)
        {
            const auto db = juce::Decibels::gainToDecibels(gain, floorDb);
            return area.getHeight() * juce::jlimit(0.f, 1.f, 1 - db / floorDb);
        };
        const auto drawBar = [&](float x, float height, juce::Colour colour)
        {
            if (height < .5f)
                return;
            const juce::Rectangle<float> bar(x - width * .5f, bottom - height, width, height);
            // Each repeat glows on the phosphor like a trace.
            for (const auto& [spread, alpha] : { std::pair { 4.f, .08f }, std::pair { 2.f, .16f } })
            {
                g.setColour(colour.withMultipliedAlpha(alpha));
                g.fillRoundedRectangle(bar.expanded(spread, spread * .5f), 1 + spread);
            }
            g.setColour(colour);
            g.fillRoundedRectangle(bar, 1);
        };

        const auto colour = data.active ? accent : muted.withAlpha(.42f);
        const auto mix = juce::jlimit(0.f, 1.f, data.mix);
        drawBar(first, heightFor(1 - mix), muted.withAlpha(data.active ? .8f : .42f));
        if (mix > 0)
        {
            juce::Path decay;
            auto level = mix;
            for (int repeat = 1; repeat * ms <= span * 1.0001f; ++repeat)
            {
                const auto x = first + pixelsPerMs * ms * repeat;
                const auto height = heightFor(level);
                const juce::Point<float> top(x, bottom - height);
                repeat == 1 ? decay.startNewSubPath(top) : decay.lineTo(top);
                drawBar(x, height, colour);
                level *= juce::jlimit(0.f, 1.f, data.feedback);
            }
            g.setColour(colour.withAlpha(.35f));
            g.strokePath(decay, juce::PathStrokeType(1.f));
        }
        retro::crtGlass(g, getLocalBounds().toFloat().reduced(.5f), 6);
    }

private:
    DelayPreview data { 0.0f, 0.0f, 0.0f, false };
};

// The Stretch program drawn over one pass: shaded where audio slows down, the ratio curve on a log scale above
// the LIVE rail, and a playhead following the audio thread. The compact form is the screen on the Stretch card.
// Once attached to its parameters, the START and LAND bars can be dragged along the map: they snap to sixteenths
// of the program (Alt places them freely) and a double-click returns a bar to its default.
class StretchMap final : public juce::Component
{
public:
    explicit StretchMap(bool strip) : compact(strip) { setInterceptsMouseClicks(false, false); }

    void attach(juce::RangedAudioParameter* start, juce::RangedAudioParameter* land)
    {
        bars = { start, land };
        setInterceptsMouseClicks(start != nullptr && land != nullptr, false);
    }

    void update(StretchPreview preview)
    {
        if (dragging >= 0)
            preview.start = data.start, preview.land = data.land;
        if (data != preview)
        {
            data = preview;
            repaint();
        }
    }

    void mouseMove(const juce::MouseEvent& event) override { setHovered(barAt(event.position)); }
    void mouseExit(const juce::MouseEvent&) override { setHovered(-1); }

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragging = barAt(event.position);
        if (dragging < 0)
            return;
        bars[static_cast<size_t>(dragging)]->beginChangeGesture();
        moveBar(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragging >= 0)
            moveBar(event);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (dragging >= 0)
            bars[static_cast<size_t>(dragging)]->endChangeGesture();
        dragging = -1;
        setHovered(barAt(event.position));
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        const auto bar = barAt(event.position);
        if (bar < 0)
            return;
        auto* parameter = bars[static_cast<size_t>(bar)];
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
        parameter->endChangeGesture();
        position(bar) = parameter->convertFrom0to1(parameter->getDefaultValue()) * .01f;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (compact)
            paintScreen(g, getLocalBounds().toFloat().reduced(.5f));
        else
            paintPanel(g, getLocalBounds().toFloat().reduced(.5f));
    }

private:
    static constexpr float maximumRatio = 20.f;
    static constexpr int startBar = 0, landBar = 1;

    float& position(int bar) { return bar == startBar ? data.start : data.land; }
    float position(int bar) const { return bar == startBar ? data.start : data.land; }

    // The plotting area, shared by painting and by dragging the bars.
    juce::Rectangle<float> graphBounds() const
    {
        auto area = getLocalBounds().toFloat().reduced(.5f);
        if (compact)
            return area.reduced(7, 5).withTrimmedTop(13);
        area.reduce(12, 6);
        area.removeFromTop(24);
        area.removeFromRight(26);
        return area.withTrimmedBottom(1);
    }

    int barAt(juce::Point<float> point) const
    {
        if (bars[0] == nullptr || bars[1] == nullptr)
            return -1;
        const auto graph = graphBounds();
        if (point.y < graph.getY() - 6 || point.y > graph.getBottom() + 4)
            return -1;
        auto best = -1;
        auto nearest = compact ? 7.f : 8.f;
        // LAND is checked first, so when the bars meet the one pulled out is the end of the region.
        for (const auto bar : { landBar, startBar })
        {
            const auto x = graph.getX() + graph.getWidth() * position(bar);
            if (const auto distance = std::abs(point.x - x); distance < nearest)
            {
                nearest = distance;
                best = bar;
            }
        }
        return best;
    }

    void setHovered(int bar)
    {
        if (std::exchange(hovered, bar) != bar)
        {
            setMouseCursor(bar >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void moveBar(const juce::MouseEvent& event)
    {
        const auto graph = graphBounds();
        auto amount = juce::jlimit(0.f, 1.f, (event.position.x - graph.getX()) / juce::jmax(1.f, graph.getWidth()));
        if (!event.mods.isAltDown())
            amount = std::round(amount * 16.f) / 16.f;
        auto* parameter = bars[static_cast<size_t>(dragging)];
        parameter->setValueNotifyingHost(parameter->convertTo0to1(amount * 100.f));
        position(dragging) = amount;
        repaint();
    }

    // A draggable bar: a line through the map with a lettered tab at its top.
    void drawBar(juce::Graphics& g, juce::Rectangle<float> graph, int bar, float alpha) const
    {
        const auto colour = (bar == startBar ? amber : cream).withAlpha(alpha);
        const auto lit = bar == hovered || bar == dragging;
        const auto x = graph.getX() + graph.getWidth() * position(bar);
        g.setColour(colour.withMultipliedAlpha(lit ? 1.f : .8f));
        g.fillRect(x - (lit ? 1.f : .6f), graph.getY(), lit ? 2.f : 1.2f, graph.getHeight());
        const auto tab = juce::Rectangle<float>(compact ? 12.f : 14.f, compact ? 10.f : 11.f)
                             .withCentre({ x, graph.getY() + (compact ? 4.f : 5.f) });
        g.setColour(juce::Colours::black.withAlpha(.5f * alpha));
        g.fillRoundedRectangle(tab.translated(0, 1), 2);
        g.setColour(lit ? colour.brighter(.25f) : colour);
        g.fillRoundedRectangle(tab, 2);
        g.setColour(juce::Colour(0xff15132b).withAlpha(alpha));
        g.setFont(font(compact ? 9.f : 10.f, true));
        g.drawText(bar == startBar ? "S" : "L", tab, juce::Justification::centred, false);
    }

    juce::String draggingReadout() const
    {
        return juce::String(dragging == startBar ? "START " : "LAND ") + juce::String(juce::roundToInt(position(dragging) * 100.f)) + "%";
    }

    // Height of a ratio on the map: 0 at LIVE (1x), 1 at 20x.
    static float lift(float ratio) { return ratio <= 1 ? 0.f : std::log(ratio) / std::log(maximumRatio); }

    float ratioAt(float position) const
    {
        StretchControls controls;
        controls.ratio = data.ratio;
        controls.start = data.start;
        controls.land = data.land;
        controls.shape = data.shape;
        return TimeStretch::ratioAt(controls, position);
    }

    // Shades the columns where the program stretches and returns the outline of the ratio curve, on the same
    // fixed 1x-20x log scale everywhere so a height always means the same ratio.
    juce::Path shadeAndTrace(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour shade) const
    {
        const auto columns = juce::jmax(24, juce::roundToInt(area.getWidth()));
        juce::Path trace;
        g.setColour(shade);
        for (int i = 0; i <= columns; ++i)
        {
            const auto position = static_cast<float>(i) / static_cast<float>(columns);
            const auto ratio = ratioAt(juce::jmin(position, .9999f));
            const auto x = area.getX() + area.getWidth() * position;
            const auto y = area.getBottom() - lift(ratio) * area.getHeight();
            if (ratio > 0 && i < columns)
                g.fillRect(x, area.getY(), area.getWidth() / static_cast<float>(columns) + .5f, area.getHeight());
            i == 0 ? trace.startNewSubPath(x, y) : trace.lineTo(x, y);
        }
        return trace;
    }

    // The card's screen: a sunken indigo display with the curve scaled to the set ratio, START and LAND
    // markers, the playhead and a small status readout.
    void paintScreen(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        g.setColour(juce::Colours::white.withAlpha(.1f));
        g.drawRoundedRectangle(area.translated(0, 1), 5, 1);
        g.setColour(juce::Colour(0xff0b0a1a));
        g.fillRoundedRectangle(area, 5);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(.55f), 0, area.getY(),
                                               juce::Colours::transparentBlack, 0, area.getY() + 10, false));
        g.fillRoundedRectangle(area, 5);
        g.setColour(juce::Colours::black.withAlpha(.7f));
        g.drawRoundedRectangle(area, 5, 1);

        const auto alpha = data.active ? 1.f : .35f;
        const auto graph = graphBounds();
        for (int i = 1; i < 8; ++i)
        {
            g.setColour(juce::Colour(0xff5a55a6).withAlpha(i % 4 == 0 ? .35f : .16f));
            g.drawVerticalLine(juce::roundToInt(graph.getX() + graph.getWidth() * static_cast<float>(i) / 8.f),
                               graph.getY(), graph.getBottom());
        }
        for (const auto ratio : { 2.f, 4.f, 8.f, 16.f })
        {
            g.setColour(juce::Colour(0xff5a55a6).withAlpha(ratio == 4.f || ratio == 16.f ? .3f : .14f));
            g.drawHorizontalLine(juce::roundToInt(graph.getBottom() - lift(ratio) * graph.getHeight()),
                                 graph.getX(), graph.getRight());
        }

        {
            const juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(graph.expanded(0, 1).toNearestInt());
            auto trace = shadeAndTrace(g, graph, amber.withAlpha(.07f * alpha));
            auto fill = trace;
            fill.lineTo(graph.getBottomRight());
            fill.lineTo(graph.getBottomLeft());
            fill.closeSubPath();
            g.setColour(amber.withAlpha(.24f * alpha));
            g.fillPath(fill);
            retro::crtTrace(g, trace, amber.withAlpha(alpha), 1.5f);
        }
        g.setColour(cream.withAlpha(.45f));
        g.drawLine(graph.getX(), graph.getBottom(), graph.getRight(), graph.getBottom(), 1.f);

        const auto x = graph.getX() + graph.getWidth() * data.phase;
        g.setColour(cream.withAlpha(.8f * alpha));
        g.drawLine(x, graph.getY() - 2, x, graph.getBottom(), 1.f);
        drawBar(g, graph, startBar, alpha);
        drawBar(g, graph, landBar, alpha);
        const auto y = graph.getBottom() - lift(data.stretching ? data.current : 1.f) * graph.getHeight();
        g.setColour(data.stretching ? amber.brighter(.3f) : cream);
        g.fillEllipse(x - 3, y - 3, 6, 6);

        const auto status = area.reduced(7, 3).removeFromTop(12);
        g.setFont(readoutFont(10.5f));
        g.setColour(dragging >= 0 ? (dragging == startBar ? amber : cream) : cream.withAlpha(.55f));
        g.drawText(dragging >= 0 ? draggingReadout() : juce::String(data.ratio, 1) + "x max", status,
                   juce::Justification::centredLeft);
        g.setColour(data.stretching ? amber : cream.withAlpha(.8f));
        g.drawText(data.stretching ? juce::String(data.current, 2) + "x  " + juce::String(juce::roundToInt(data.debtSeconds * 1000)) + " ms"
                                   : juce::String("LIVE"),
                   status, juce::Justification::centredRight);
        paintTube(g, area, 5);
    }

    // The monitor's glass, and tracking trouble that grows the harder the program stretches the audio.
    void paintTube(juce::Graphics& g, juce::Rectangle<float> area, float radius) const
    {
        const auto strain = data.active && data.stretching ? .2f + .7f * lift(data.current) : 0.f;
        retro::trackingNoise(g, area, radius, strain, juce::Time::getMillisecondCounterHiRes() * .001, seed);
        retro::crtGlass(g, area, radius);
    }

    void paintPanel(juce::Graphics& g, juce::Rectangle<float> area) const
    {
        g.setColour(juce::Colour(0xff0c0b1d).withAlpha(.92f));
        g.fillRoundedRectangle(area, 6);
        g.setColour(border);
        g.drawRoundedRectangle(area, 6, 1);
        area.reduce(12, 6);
        auto heading = area.removeFromTop(20);
        g.setColour(muted);
        g.setFont(font(13, true));
        if (dragging >= 0)
            g.setColour(dragging == startBar ? amber : cream);
        g.drawText(dragging >= 0 ? draggingReadout() : juce::String("Stretch map"), heading,
                   juce::Justification::centredLeft);
        g.setFont(readoutFont(13.f));
        g.setColour(data.stretching ? amber : cream.withAlpha(.8f));
        const auto status = data.stretching
            ? juce::String(data.current, 2) + "x   debt " + juce::String(juce::roundToInt(data.debtSeconds * 1000)) + " ms"
            : juce::String("LIVE");
        g.drawText(status, heading, juce::Justification::centredRight);

        area.removeFromTop(4);
        const auto labels = area.removeFromRight(26);
        const auto graph = graphBounds();
        for (int i = 0; i <= 8; ++i)
        {
            g.setColour(border.withAlpha(i % 4 == 0 ? .7f : .35f));
            g.drawVerticalLine(juce::roundToInt(graph.getX() + graph.getWidth() * static_cast<float>(i) / 8.f),
                               graph.getY(), graph.getBottom());
        }
        g.setFont(readoutFont(10.f));
        for (const auto ratio : { 4.f, 16.f })
        {
            const auto y = graph.getBottom() - lift(ratio) * graph.getHeight();
            g.setColour(border.withAlpha(.45f));
            g.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
            g.setColour(muted.withAlpha(.8f));
            g.drawText(juce::String(static_cast<int>(ratio)) + "x", labels.withY(y - 6).withHeight(12),
                       juce::Justification::centredRight);
        }
        g.setColour(muted.withAlpha(.8f));
        g.drawText("LIVE", labels.withY(graph.getBottom() - 10).withHeight(12), juce::Justification::centredRight);

        const auto alpha = data.active ? 1.f : .4f;
        auto trace = shadeAndTrace(g, graph, amber.withAlpha(.08f * alpha));
        auto fill = trace;
        fill.lineTo(graph.getBottomRight());
        fill.lineTo(graph.getBottomLeft());
        fill.closeSubPath();
        g.setColour(amber.withAlpha(.22f * alpha));
        g.fillPath(fill);
        retro::crtTrace(g, trace, amber.withAlpha(alpha), 1.6f);
        g.setColour(cream.withAlpha(.6f));
        g.drawLine(graph.getX(), graph.getBottom(), graph.getRight(), graph.getBottom(), 1.2f);

        const auto x = graph.getX() + graph.getWidth() * data.phase;
        g.setColour(cream.withAlpha(.85f));
        g.drawLine(x, graph.getY(), x, graph.getBottom(), 1.2f);
        drawBar(g, graph, startBar, alpha);
        drawBar(g, graph, landBar, alpha);
        const auto y = graph.getBottom() - lift(data.stretching ? data.current : 1.f) * graph.getHeight();
        g.setColour(data.stretching ? amber.brighter(.3f) : cream);
        g.fillEllipse(x - 3.5f, y - 3.5f, 7, 7);
        paintTube(g, getLocalBounds().toFloat().reduced(.5f), 6);
    }

    const juce::Colour amber { 0xffffb347 }, cream { retro::palette::cream };
    const bool compact;
    StretchPreview data;
    const juce::uint32 seed = static_cast<juce::uint32>(juce::Random::getSystemRandom().nextInt());
    std::array<juce::RangedAudioParameter*, 2> bars {};
    int hovered = -1, dragging = -1;
};

const juce::Colour cream = retro::palette::cream;
const juce::Colour charcoal = retro::palette::charcoal;

using Area = juce::Rectangle<int>;

void drawOpticallyCentred(juce::Graphics& g, const juce::String& text, const juce::Font& typeface, Area area)
{
    if (text.isEmpty() || area.isEmpty())
        return;

    juce::GlyphArrangement glyphs;
    glyphs.addFittedText(typeface, text, static_cast<float>(area.getX()), static_cast<float>(area.getY()),
                         static_cast<float>(area.getWidth()), static_cast<float>(area.getHeight()),
                         juce::Justification::centred, 1, .76f);
    juce::Path outlines;
    glyphs.createPath(outlines);
    const auto inkBounds = outlines.getBounds();
    if (!inkBounds.isEmpty())
    {
        const auto offset = area.toFloat().getCentre() - inkBounds.getCentre();
        glyphs.moveRangeOfGlyphs(0, -1, offset.x, offset.y);
    }
    glyphs.draw(g);
}

constexpr int slotWidth = 248, slotHeight = 150;
constexpr int cavityInset = 13, cavityTop = 35, cavityBottom = 8;
constexpr int drawerOverlap = 4;
constexpr float compactDividerPosition = 90.f / 513.f; // Engraved rule across every compact face.
constexpr float cartridgeAspect = static_cast<float>(slotWidth - 2 * cavityInset)
    / (slotHeight - cavityTop - cavityBottom);

class CartridgeImages
{
public:
    enum Part { slot };
    enum Sprite { valueDisplay, selector, lampOn, lampOff, track, thumb };
    enum Key { generate, prev, next, options, controls, close, off, on };

    void draw(juce::Graphics& g, Part, Area bounds) const { retro::dock(g, bounds); }
    void drawHeader(juce::Graphics& g, Area area) const { retro::header(g, area); }
    void drawRack(juce::Graphics& g, Area area) const { retro::rackFrame(g, area); }
    void drawConnector(juce::Graphics& g, Area area, float opacity = 1.f) const { retro::connector(g, area, opacity); }
    void drawPanel(juce::Graphics& g, Area area, size_t effect) const
    {
        retro::face(g, area, effect, compactDividerPosition);
    }

    Area fitInspector(Area area, size_t) const { return fit(area, 760.f / 397.f); }
    void drawInspector(juce::Graphics& g, Area area, size_t effect) const { retro::inspectorPlate(g, area, effect); }
    void drawInspectorClose(juce::Graphics& g, Area area, float opacity) const
    {
        retro::key(g, retro::Key::inspectorClose, area.reduced(2), opacity);
    }
    void drawInspectorValue(juce::Graphics& g, Area area) const { retro::valueWell(g, area.reduced(1), false); }
    Area inspectorSwitchBounds(Area area) const { return fit(area.reduced(1), 751.f / 157.f); }
    void drawInspectorSwitch(juce::Graphics& g, Area area, bool on, float opacity) const
    {
        retro::inspectorSwitch(g, inspectorSwitchBounds(area), on, opacity);
    }

    static juce::Colour panelText(size_t effect)
    {
        switch (effect)
        {
            case 1: // Tremolo
            case 3: // Resampler
            case 5: // Delay
            case 6: // Reverse
            case 7: // Reverb
            case 8: // Stretch
            case 9: // VHS
            case 10: // Grain
                return cream;
            default:
                return metalInk;
        }
    }

    void drawAdd(juce::Graphics& g, Area area, float opacity) const { retro::addFrame(g, area, opacity); }
    void drawPreset(juce::Graphics& g, Area area) const { retro::presetHousing(g, area); }

    void drawButtonFace(juce::Graphics& g, Key key, Area area, float opacity = 1.f) const
    {
        static constexpr std::array<retro::Key, 8> faces { retro::Key::generate, retro::Key::previous,
            retro::Key::next, retro::Key::options, retro::Key::controls, retro::Key::close,
            retro::Key::toggleOff, retro::Key::toggleOn };
        retro::key(g, faces[static_cast<size_t>(key)], area, opacity);
    }

    void drawSignal(juce::Graphics& g, Area area, const std::array<float, 2>& peaks) const
    {
        retro::signal(g, area, peaks);
    }

    void drawDock(juce::Graphics& g, Area area, bool cartridge, float opacity = 1) const
    {
        retro::port(g, area, cartridge, opacity);
    }

    void drawSprite(juce::Graphics& g, Sprite sprite, Area area, float opacity = 1) const
    {
        retro::lamp(g, area, sprite != lampOff, opacity);
    }

    void drawKey(juce::Graphics& g, Sprite sprite, Area area, float opacity = 1) const
    {
        if (sprite == valueDisplay)
            retro::valueWell(g, area, true);
        else
            retro::scrollTrack(g, area, sprite == thumb, opacity);
    }

    void drawSelector(juce::Graphics& g, Area area, bool focused, float opacity = 1.f) const
    {
        const auto chevron = scaledCapWidths(area, selectorReferenceHeight, selectorLeftWidth, selectorRightWidth)[1];
        retro::selector(g, area, focused, chevron, opacity);
    }

    static Area selectorTextArea(Area area)
    {
        const auto right = scaledCapWidths(area, selectorReferenceHeight, selectorLeftWidth,
                                           selectorRightWidth)[1];
        // The text keeps clear of the chevron by the same margin on both sides, so it sits on the selector's own
        // centre line, under its caption; a long name still squeezes to fit.
        const auto inset = juce::jmax(juce::jmax(4, area.getHeight() / 6), right + 2);
        if (area.getWidth() - 2 * inset >= area.getWidth() / 2)
            return area.reduced(inset, 2);
        const auto leftInset = juce::jmax(4, area.getHeight() / 6);
        return area.withTrimmedLeft(leftInset).withTrimmedRight(right + 2).reduced(0, 2);
    }

    void drawSliderTrack(juce::Graphics& g, Sprite sprite, Area area, bool, float opacity = 1) const
    {
        retro::scrollTrack(g, area, sprite == thumb, opacity);
    }

    void drawDeck(juce::Graphics& g, Area area) const { retro::deck(g, area); }
    void drawShelf(juce::Graphics& g, Area area) const { retro::shelf(g, area); }

    static Area fitCartridge(Area available) { return fit(available, cartridgeAspect); }

    void drawKnob(juce::Graphics& g, juce::Point<float> centre, int diameter, float angle,
                  size_t effect = editorModuleCount) const
    {
        retro::knob(g, centre, static_cast<float>(diameter), angle, effect);
    }

    void drawCartridge(juce::Graphics& g, Area area, int type, bool highlighted, float opacity = 1) const
    {
        retro::seatedCartridge(g, area, static_cast<size_t>(type), highlighted ? opacity * .88f : opacity);
    }

    void drawSeatShadow(juce::Graphics& g, Area cavity) const { retro::seatShadow(g, cavity); }

private:
    static constexpr int selectorReferenceHeight = 246, selectorLeftWidth = 76, selectorRightWidth = 243;

    static Area fit(Area available, float aspectRatio)
    {
        const auto width = juce::jmin(available.getWidth(), juce::roundToInt(available.getHeight() * aspectRatio));
        return available.withSizeKeepingCentre(width, juce::roundToInt(width / aspectRatio));
    }

    static std::array<int, 2> scaledCapWidths(Area area, int sourceHeight, int left, int right)
    {
        const auto scale = juce::jmin(static_cast<float>(area.getHeight()) / sourceHeight,
                                     static_cast<float>(area.getWidth() - 1) / (left + right));
        return { juce::roundToInt(left * scale), juce::roundToInt(right * scale) };
    }

    juce::SharedResourcePointer<retro::Grain> grain; // Keeps the procedural texture alive.
};

class ValueDisplay final : public juce::Label
{
public:
    ValueDisplay(const CartridgeImages& artwork, juce::Slider& slider)
        : art(artwork), compact(static_cast<bool>(slider.getProperties()["cassette_compact"]))
    {
        setComponentID(slider.getComponentID() + "_value");
        setJustificationType(juce::Justification::centred);
        setKeyboardType(juce::TextInputTarget::decimalKeyboard);
        setFont(readoutFont(12.f));
        setBorderSize({ 1, 5, 1, 5 });
        setColour(backgroundColourId, juce::Colours::transparentBlack);
        setColour(outlineColourId, juce::Colours::transparentBlack);
        setColour(textColourId, cream);
        setColour(juce::TextEditor::textColourId, cream);
        setColour(juce::TextEditor::backgroundColourId, panel);
        setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TextEditor::focusedOutlineColourId, accent.withAlpha(.65f));
        getProperties().set("cassette_readout", true);
    }

    void paint(juce::Graphics& g) override
    {
        if (compact)
            art.drawKey(g, CartridgeImages::valueDisplay, getLocalBounds());
        else
            art.drawInspectorValue(g, getLocalBounds());
        juce::Label::paint(g);

        // The knob above stands taller than this display, so its shadow falls across it too; the same shadow,
        // painted again over the display, carries on seamlessly from the panel.
        if (auto* slider = dynamic_cast<juce::Slider*>(getParentComponent()))
        {
            const auto layout = slider->getLookAndFeel().getSliderLayout(*slider);
            const auto knob = knobArea(*slider, layout.sliderBounds).translated(static_cast<float>(-getX()),
                                                                               static_cast<float>(-getY()));
            if (knob.getWidth() >= 12)
                droppedKnobShadow(g, knob.getCentre(), knob.getWidth(), knobDrop(*slider));
        }
    }

private:
    const CartridgeImages& art;
    const bool compact;
};

class CartridgeLook final : public juce::LookAndFeel_V4
{
public:
    explicit CartridgeLook(const CartridgeImages& artwork) : art(artwork)
    {
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::textColourId, cream);
        // Not quite opaque, so menu windows are transparent and their rounded corners show the app behind them.
        setColour(juce::PopupMenu::backgroundColourId, panel.withAlpha(.98f));
        setColour(juce::PopupMenu::textColourId, ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, muted.darker(.52f));
        setColour(juce::PopupMenu::highlightedTextColourId, ink);
        setColour(juce::TextEditor::highlightColourId, accent.withAlpha(.36f));
        setColour(juce::CaretComponent::caretColourId, cream);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(13.5f); }
    juce::Font getPopupMenuFont() override { return font(15.f); }

    int getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) override { return 5; }

    void drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                            const juce::PopupMenu::Options&) override
    {
        retro::menuPanel(g, { 0, 0, width, height });
    }

    void drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area, bool highlighted,
                                      const juce::PopupMenu::Item& item, const juce::PopupMenu::Options&) override
    {
        retro::menuItem(g, area, item.text, item.isSeparator, item.isEnabled, highlighted, item.isTicked,
                        item.subMenu != nullptr);
    }

    void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool separator, int, int& width, int& height,
                                              const juce::PopupMenu::Options&) override
    {
        if (separator)
        {
            width = 40;
            height = 9;
            return;
        }
        height = 27;
        width = juce::roundToInt(juce::GlyphArrangement::getStringWidth(font(15.f), text)) + 66;
    }
    juce::Label* createSliderTextBox(juce::Slider& slider) override { return new ValueDisplay(art, slider); }

    juce::Slider::SliderLayout getSliderLayout(juce::Slider& slider) override
    {
        auto layout = juce::LookAndFeel_V4::getSliderLayout(slider);
        if (slider.getTextBoxPosition() == juce::Slider::TextBoxBelow)
            layout.sliderBounds = layout.sliderBounds.withTrimmedBottom(
                static_cast<bool>(slider.getProperties()["cassette_hero"]) ? 0 : 6);
        return layout;
    }

    void positionComboBoxText(juce::ComboBox& combo, juce::Label& text) override
    {
        text.setBounds(CartridgeImages::selectorTextArea(combo.getLocalBounds()));
        text.setJustificationType(juce::Justification::centred);
        text.setBorderSize({});
        text.setFont(getComboBoxFont(combo));
        text.setMinimumHorizontalScale(.72f);
        text.getProperties().set("cassette_header_text", combo.getProperties()["cassette_header_text"]);
    }

    void drawLabel(juce::Graphics& g, juce::Label& text) override
    {
        if (text.isBeingEdited())
        {
            juce::LookAndFeel_V4::drawLabel(g, text);
            return;
        }

        if (const auto* card = text.getProperties().getVarPointer("cassette_title"))
        {
            retro::title(g, text.getLocalBounds(), text.getText(), static_cast<size_t>(static_cast<int>(*card)));
            return;
        }

        const auto colour = text.findColour(juce::Label::textColourId).withMultipliedAlpha(text.isEnabled() ? 1.f : .5f);
        if (static_cast<bool>(text.getProperties()["cassette_header_text"]))
        {
            drawPrinted(g, colour, [&] { drawOpticallyCentred(g, text.getText(), text.getFont(), text.getLocalBounds()); });
            return;
        }

        g.fillAll(text.findColour(juce::Label::backgroundColourId));
        const auto typeface = getLabelFont(text);
        const auto area = getLabelBorderSize(text).subtractedFrom(text.getLocalBounds());
        const auto lines = juce::jmax(1, static_cast<int>(static_cast<float>(area.getHeight()) / typeface.getHeight()));
        g.setFont(typeface);
        const auto draw = [&]
        {
            g.drawFittedText(text.getText(), area, text.getJustificationType(), lines, text.getMinimumHorizontalScale());
        };
        if (static_cast<bool>(text.getProperties()["cassette_readout"]))
            drawGlowing(g, colour, draw);
        else
            drawPrinted(g, colour, draw);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool down, int, int, int, int,
                      juce::ComboBox& combo) override
    {
        const bool focused = down || combo.isPopupActive() || combo.hasKeyboardFocus(true);
        art.drawSelector(g, Area(width, height), focused, combo.isEnabled() ? 1.f : .45f);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool over, bool down) override
    {
        const auto face = buttonFace(button);
        const auto bounds = button.getLocalBounds();
        const auto opacity = !button.isEnabled() ? .45f : down ? .82f : over ? .94f : 1.0f;
        switch (face)
        {
            case ButtonFace::lamp:
                // A lamp held dark by the power-on sequence stays off whatever the parameter says.
                art.drawSprite(g, button.getToggleState() && !static_cast<bool>(button.getProperties()["cassette_dark"])
                                      ? CartridgeImages::lampOn : CartridgeImages::lampOff,
                               bounds, opacity);
                break;
            case ButtonFace::presetName: break;
            case ButtonFace::selector:
                art.drawSelector(g, bounds, down || button.hasKeyboardFocus(true), opacity);
                return;
            case ButtonFace::previous: art.drawButtonFace(g, CartridgeImages::prev, bounds, opacity); break;
            case ButtonFace::next: art.drawButtonFace(g, CartridgeImages::next, bounds, opacity); break;
            case ButtonFace::options: art.drawButtonFace(g, CartridgeImages::options, bounds, opacity); break;
            case ButtonFace::controls:
                // The card's input for the knobs in its full controls; clicking it opens them.
                paintPanelSocket(g, bounds.toFloat().getCentre(), juce::jmin(bounds.getWidth(), bounds.getHeight()) * .72f,
                                 opacity, button.isOver() || button.isDown());
                break;
            case ButtonFace::close: art.drawButtonFace(g, CartridgeImages::close, bounds, opacity); break;
            case ButtonFace::inspectorClose: art.drawInspectorClose(g, bounds, opacity); break;
            case ButtonFace::inspectorToggle:
                art.drawInspectorSwitch(g, bounds, button.getToggleState(), opacity);
                break;
            case ButtonFace::generate: art.drawButtonFace(g, CartridgeImages::generate, bounds, opacity); break;
            case ButtonFace::toggle:
                art.drawButtonFace(g, button.getToggleState() ? CartridgeImages::on : CartridgeImages::off, bounds, opacity);
                break;
        }
        drawFocus(g, button);
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool over, bool) override
    {
        const auto face = buttonFace(button);
        if (face == ButtonFace::inspectorToggle)
        {
            drawInspectorSwitchText(g, button);
            return;
        }
        if (face != ButtonFace::selector && face != ButtonFace::presetName)
            return;
        const auto bounds = button.getLocalBounds();
        const auto textArea = face == ButtonFace::selector ? CartridgeImages::selectorTextArea(bounds)
                                                           : bounds.reduced(4, 0);
        const auto colour = over ? juce::Colours::white : cream;
        if (face == ButtonFace::selector)
        {
            drawPrinted(g, colour, [&] { drawOpticallyCentred(g, button.getButtonText(), font(13.5f), textArea); });
            return;
        }

        // The preset name sits behind glass like a backlit display.
        g.setFont(font(15.f));
        drawGlowing(g, colour, [&]
            { g.drawFittedText(button.getButtonText(), textArea, juce::Justification::centred, 1, .75f); });
    }

    // Light text gets a cast shadow; dark ink on a light panel gets a letterpress highlight below it.
    template <typename Draw>
    static void drawPrinted(juce::Graphics& g, juce::Colour colour, Draw&& draw)
    {
        {
            const juce::Graphics::ScopedSaveState state(g);
            g.addTransform(juce::AffineTransform::translation(0, 1.f));
            g.setColour(colour.getPerceivedBrightness() > .5f ? juce::Colours::black.withAlpha(.6f * colour.getFloatAlpha())
                                                               : juce::Colours::white.withAlpha(.28f * colour.getFloatAlpha()));
            draw();
        }
        g.setColour(colour);
        draw();
    }

    template <typename Draw>
    static void drawGlowing(juce::Graphics& g, juce::Colour colour, Draw&& draw)
    {
        for (const auto& offset : { juce::Point<float>(-1, 0), { 1, 0 }, { 0, -1 }, { 0, 1 } })
        {
            const juce::Graphics::ScopedSaveState state(g);
            g.addTransform(juce::AffineTransform::translation(offset));
            g.setColour(colour.withMultipliedAlpha(.16f));
            draw();
        }
        g.setColour(colour);
        draw();
    }

    void drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                       int thumbStart, int thumbSize, bool over, bool down) override
    {
        const Area groove(x, y, w, h);
        art.drawSliderTrack(g, CartridgeImages::track, groove, vertical, .72f);
        if (thumbSize > 0)
        {
            const auto cap = vertical ? Area(x, thumbStart, w, thumbSize) : Area(thumbStart, y, thumbSize, h);
            art.drawSliderTrack(g, CartridgeImages::thumb, cap, vertical, down || over ? 1.f : .82f);
        }
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float proportion, float start, float end, juce::Slider& slider) override
    {
        const auto& properties = slider.getProperties();
        const auto hero = static_cast<bool>(properties["cassette_hero"]);
        const auto knob = knobArea(slider, { x, y, width, height });
        const auto diameter = juce::roundToInt(knob.getWidth());
        if (diameter < 12)
            return;

        const auto effect = static_cast<int>(properties.getWithDefault("cassette_effect", static_cast<int>(editorModuleCount)));
        const auto centre = knob.getCentre();
        auto shown = proportion;
        if (const auto* sweep = properties.getVarPointer("cassette_sweep"))
        {
            // Power-on self test: the pointer swings up to its end stop, then falls back onto its setting.
            const auto progress = static_cast<float>(*sweep);
            constexpr float rise = .42f;
            if (progress < rise)
            {
                const auto u = progress / rise;
                shown = u * u * (3.f - 2.f * u);
            }
            else
                shown = 1.f + (proportion - 1.f) * easeOutBack((progress - rise) / (1.f - rise));
            shown = juce::jlimit(0.f, 1.f, shown);
        }
        const auto angle = std::lerp(start, end, shown);
        const auto drop = knobDrop(slider);
        const auto lifted = juce::roundToInt(static_cast<float>(diameter) * (1.f + .3f * drop));
        const auto paintKnob = [&](juce::Graphics& target)
        {
            if (hero)
            {
                const auto ink = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(properties["cassette_ink"])));
                retro::knobScale(target, centre, diameter * .5f + 1.f, start, end, ink);
            }
            art.drawKnob(target, centre, lifted, angle, static_cast<size_t>(effect));
        };

        // While a knob animates it is drawn directly; at rest its picture is kept until it turns or resizes,
        // since the turned-metal finishes are costly and the rack repaints often (scrolling, meters, drops).
        if (properties.contains("cassette_sweep") || drop > 0)
        {
            paintKnob(g);
            return;
        }
        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const auto pad = juce::roundToInt(static_cast<float>(diameter) * .2f) + 8;
        const auto area = juce::Rectangle<int>(x, y, width, height).getUnion(knob.toNearestInt()).expanded(pad);
        auto* cached = dynamic_cast<KnobPicture*>(properties["cassette_knob_picture"].getObject());
        if (cached == nullptr || cached->angle != angle || cached->area != area || std::abs(cached->scale - scale) > .001f)
        {
            auto picture = juce::ReferenceCountedObjectPtr<KnobPicture>(new KnobPicture());
            picture->angle = angle;
            picture->area = area;
            picture->scale = scale;
            picture->image = juce::Image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt(static_cast<float>(area.getWidth()) * scale)),
                                         juce::jmax(1, juce::roundToInt(static_cast<float>(area.getHeight()) * scale)), true);
            juce::Graphics painted(picture->image);
            painted.addTransform(juce::AffineTransform::translation(static_cast<float>(-area.getX()),
                                                                    static_cast<float>(-area.getY()))
                                     .scaled(scale));
            paintKnob(painted);
            slider.getProperties().set("cassette_knob_picture", juce::var(picture.get()));
            cached = picture.get();
        }
        g.drawImageTransformed(cached->image, juce::AffineTransform::scale(1.f / cached->scale)
                                                  .translated(static_cast<float>(cached->area.getX()),
                                                              static_cast<float>(cached->area.getY())));
    }

    // A knob's last drawn picture, kept on its slider.
    struct KnobPicture final : juce::ReferenceCountedObject
    {
        juce::Image image;
        juce::Rectangle<int> area;
        float angle = 0, scale = 1;
    };

private:
    void drawInspectorSwitchText(juce::Graphics& g, const juce::Button& button) const
    {
        auto area = art.inspectorSwitchBounds(button.getLocalBounds()).reduced(5, 0);
        const auto left = area.removeFromLeft(area.getWidth() / 2);
        const bool on = button.getToggleState();
        const bool stereo = static_cast<bool>(button.getProperties()["stereo_switch"]);
        g.setFont(font(12.f, true));
        g.setColour(on ? cream : metalInk);
        g.drawFittedText(stereo ? "Mono" : "Off", left, juce::Justification::centred, 1);
        g.setColour(on ? metalInk : cream);
        g.drawFittedText(stereo ? "Stereo" : "On", area, juce::Justification::centred, 1);
    }

    static ButtonFace buttonFace(juce::Button& button)
    {
        return static_cast<ButtonFace>(static_cast<int>(button.getProperties()["face"]));
    }

    // Keyboard focus ring, drawn around the visible key rather than the whole button area. Mouse clicks do not
    // take focus (see styleButton), so it only appears while navigating with the keyboard.
    void drawFocus(juce::Graphics& g, juce::Button& button) const
    {
        if (!button.hasKeyboardFocus(true))
            return;
        const auto fitted = [](juce::Rectangle<float> area, float aspect)
        {
            const auto width = juce::jmin(area.getWidth(), area.getHeight() * aspect);
            return area.withSizeKeepingCentre(width, width / aspect);
        };
        auto area = button.getLocalBounds().toFloat();
        auto radius = 4.f;
        switch (buttonFace(button))
        {
            case ButtonFace::lamp:
                area = fitted(area, 1.f);
                radius = area.getWidth() * .5f;
                break;
            case ButtonFace::inspectorToggle: area = art.inspectorSwitchBounds(button.getLocalBounds()).toFloat(); break;
            case ButtonFace::previous: area = fitted(area, retro::keyAspect(retro::Key::previous)).reduced(1); break;
            case ButtonFace::next: area = fitted(area, retro::keyAspect(retro::Key::next)).reduced(1); break;
            case ButtonFace::options: area = fitted(area, retro::keyAspect(retro::Key::options)).reduced(1); break;
            case ButtonFace::generate: area = fitted(area, retro::keyAspect(retro::Key::generate)).reduced(1); break;
            case ButtonFace::inspectorClose: area = fitted(area.reduced(2), 1.f).reduced(1); break;
            case ButtonFace::controls:
            case ButtonFace::close:
            case ButtonFace::toggle: area = fitted(area, 1.f).reduced(1); break;
            case ButtonFace::selector:
            case ButtonFace::presetName: break;
        }
        g.setColour(accent.withAlpha(.85f));
        g.drawRoundedRectangle(area.reduced(.6f), juce::jmin(radius, area.getHeight() * .5f), 1.2f);
    }

    const CartridgeImages& art;
};

class ArtworkButton final : public ModuleButton
{
public:
    ArtworkButton() { setMouseClickGrabsKeyboardFocus(false); }

    std::function<void(juce::Graphics&, Area, bool)> artwork;
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        if (artwork)
            artwork(g, getLocalBounds(), highlighted || down || hasKeyboardFocus(false));
    }
};

constexpr int drawerHeight = 242, slotGap = 10;
constexpr int slotPitch = slotWidth + slotGap;
constexpr int rackInset = 18;
constexpr int headerHeight = 43, flowHeight = 34;
constexpr int rackTop = headerHeight + flowHeight + 2;


juce::String effectPrefix(int instance)
{
    const auto type = static_cast<size_t>(instance) % modules.size();
    return (instance >= static_cast<int>(modules.size()) ? "cart2_" : "")
        + juce::String(modules[type].prefix) + "_";
}

struct FeatureKnobs
{
    std::array<const char*, 4> knobs;
};

constexpr std::array<FeatureKnobs, editorModuleCount> featured {{
    {{ "depth", "wow_rate", "flutter_rate", "flutter_amount" }},
    {{ "depth", "rate", "shape", "stereo" }},
    {{ "drive", "frequency", "tone", "mix" }},
    {{ "rate", "drive", "jitter", "post_filter" }},
    {{ "depth", "rate", "width", "mix" }},
    {{ "feedback", "saturation", "analog", "mix" }},
    {{ "forward", "pitch", "highpass", "mix" }},
    {{ "size", "decay", "pre_delay", "mix" }},
    {{ "ratio", "cycle", "mix", nullptr }},
    {{ "wear", "tracking", "wobble", nullptr }},
    {{ "size", "density", "pitch", "mix" }}
}};

juce::String controlName(juce::RangedAudioParameter& parameter, const juce::String& prefix)
{
    auto name = parameter.getName(100);
    if (prefix.startsWith("cart2_") && name.endsWith(" 2"))
        name = name.dropLastCharacters(2);
    const auto local = prefix.startsWith("cart2_") ? prefix.substring(6) : prefix;
    for (const auto& info : modules)
        if (local == juce::String(info.prefix) + "_" && name.startsWith(juce::String(info.name) + " "))
            return name.substring(static_cast<int>(std::strlen(info.name)) + 1);
    return name;
}

// The VHS card's screen: the recorder's on-screen display, PLAY while audio runs with the tape counter ticking,
// and tracking noise that follows the Tracking and Wear settings.
class VhsScreen final : public juce::Component
{
public:
    VhsScreen(juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
        : active(state.getRawParameterValue(prefix + "active")), speed(state.getRawParameterValue(prefix + "speed")),
          wear(state.getRawParameterValue(prefix + "wear")), tracking(state.getRawParameterValue(prefix + "tracking"))
    {
        setComponentID(prefix + "screen");
        setInterceptsMouseClicks(false, false);
    }

    void setTransport(float running)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        if (playing && last > 0)
            counter += (now - last) * running;
        last = now;
        playing = running > 0;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        static const juce::StringArray speeds { "SP", "LP", "EP" };
        const auto index = juce::jlimit(0, 2, juce::roundToInt(speed->load(std::memory_order_relaxed)));
        const auto on = active->load(std::memory_order_relaxed) >= .5f;
        retro::vcrScreen(g, getLocalBounds().toFloat().reduced(.5f), speeds[index], on, on && playing, counter,
                         tracking->load(std::memory_order_relaxed) * .01f, wear->load(std::memory_order_relaxed) * .01f,
                         juce::Time::getMillisecondCounterHiRes() * .001, seed);
    }

private:
    const std::atomic<float>* active;
    const std::atomic<float>* speed;
    const std::atomic<float>* wear;
    const std::atomic<float>* tracking;
    bool playing = false;
    double counter = 0, last = 0;
    const juce::uint32 seed = static_cast<juce::uint32>(juce::Random::getSystemRandom().nextInt());
};

// The Grain card's screen: the cloud as it plays. The live input enters on the right; each grain is a dot as far
// left as it reads behind the input (on the Position knob's own scale), as high as it is pitched, swelling and
// fading with its window and leaning gold or mint with its pan. Grains flash as they start and shed sparks while
// they sound, and dust drifts left with the audio flowing into the past. The screen is also the XY pad: dragging
// moves the X and Y macros, which reach whatever is patched from the X and Y jacks; a double click centres them,
// where they change nothing. Dragging an unpatched pad patches X to Position (left reaches further back in time)
// and Y to Pitch.
class GrainScreen final : public juce::Component
{
public:
    GrainScreen(juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
        : active(state.getRawParameterValue(prefix + "active")), stretch(state.getRawParameterValue(prefix + "stretch")),
          voices(state.getRawParameterValue(prefix + "voices")), stretchSpeed(state.getRawParameterValue(prefix + "stretch_speed")),
          state(state), position(state.getParameter(prefix + "position")), padX(state.getParameter("xy_x_amount")),
          padY(state.getParameter("xy_y_amount"))
    {
        setComponentID(prefix + "screen");
        setTitle("Grain cloud and XY pad: drag to move the X and Y macros, double-click to centre");
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        for (auto& mote : dust)
            mote = { random.nextFloat(), random.nextFloat(), .3f + .7f * random.nextFloat(), random.nextFloat() };
    }

    std::function<GrainPicture()> source;

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragging = true;
        patchIfUnused();
        padX->beginChangeGesture();
        padY->beginChangeGesture();
        moveTo(event.position);
    }

    void mouseDrag(const juce::MouseEvent& event) override { moveTo(event.position); }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!dragging)
            return;
        dragging = false;
        padX->endChangeGesture();
        padY->endChangeGesture();
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        for (auto* parameter : { padX, padY })
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->getDefaultValue());
            parameter->endChangeGesture();
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        const auto dt = static_cast<float>(juce::jlimit(0.0, .1, last > 0 ? now - last : 0.0));
        last = now;

        const auto area = getLocalBounds().toFloat().reduced(.5f);
        const auto on = active->load(std::memory_order_relaxed) >= .5f;
        const auto stretching = stretch->load(std::memory_order_relaxed) >= .5f;
        g.setColour(juce::Colours::white.withAlpha(.1f));
        g.drawRoundedRectangle(area.translated(0, 1), 5, 1);
        g.setColour(juce::Colour(0xff06120d));
        g.fillRoundedRectangle(area, 5);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(.55f), 0, area.getY(),
                                               juce::Colours::transparentBlack, 0, area.getY() + 10, false));
        g.fillRoundedRectangle(area, 5);
        g.setColour(juce::Colours::black.withAlpha(.7f));
        g.drawRoundedRectangle(area, 5, 1);

        const auto graph = graphBounds();
        const auto alpha = on ? 1.f : .35f;
        for (int i = 1; i < 8; ++i)
        {
            g.setColour(mint.withAlpha((i % 4 == 0 ? .22f : .09f) * alpha));
            g.drawVerticalLine(juce::roundToInt(graph.getX() + graph.getWidth() * static_cast<float>(i) / 8.f),
                               graph.getY(), graph.getBottom());
        }
        for (const auto semitones : { -12.f, 0.f, 12.f })
        {
            g.setColour(mint.withAlpha((semitones == 0 ? .3f : .12f) * alpha));
            g.drawHorizontalLine(juce::roundToInt(pitchY(semitones)), graph.getX(), graph.getRight());
        }
        g.setColour(mint.withAlpha(.6f * alpha));
        g.drawLine(graph.getRight(), graph.getY(), graph.getRight(), graph.getBottom(), 1.2f);

        int count = 0;
        {
            const juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(area.reduced(2).toNearestInt());
            paintDust(g, graph, dt, on, stretching);
            if (on && source)
            {
                const auto dots = source();
                for (size_t i = 0; i < dots.size(); ++i)
                {
                    const auto& dot = dots[i];
                    // A voice whose grain is younger than last frame has started a new one: it flashes.
                    const auto started = dot.active && (!seen[i] || dot.progress < progress[i]);
                    seen[i] = dot.active;
                    progress[i] = dot.progress;
                    if (!dot.active)
                        continue;
                    ++count;
                    const juce::Point<float> at { xFor(dot.ms), pitchY(dot.pitch) };
                    const auto colour = mint.interpolatedWith(gold, juce::jlimit(0.f, 1.f, .5f + .5f * dot.pan));
                    const auto swell = std::sin(juce::MathConstants<float>::pi * juce::jlimit(0.f, 1.f, dot.progress));
                    if (started)
                        burst(at, colour);
                    shed(at, colour, swell, dot.pitch, dt);
                    paintGrain(g, at, colour, swell);
                }
            }
            else
            {
                seen = {};
            }
            paintSparks(g, dt);
            paintHandle(g, graph, alpha);
        }

        const auto status = area.reduced(7, 3).removeFromTop(12);
        g.setFont(readoutFont(10.5f));
        if (dragging)
        {
            g.setColour(gold.withAlpha(.95f));
            g.drawText("X " + juce::String(juce::roundToInt(padX->getValue() * 200.f - 100.f)), status,
                       juce::Justification::centredLeft);
            g.setColour(mint.withAlpha(.95f));
            g.drawText("Y " + juce::String(juce::roundToInt(padY->getValue() * 200.f - 100.f)), status,
                       juce::Justification::centredRight);
        }
        else
        {
            // LIVE while the grains follow the input, STRETCH while the read head creeps behind it; then how many
            // of the allowed voices are sounding.
            g.setColour((stretching ? gold : mint).withAlpha(.75f * alpha));
            g.drawText(stretching ? "STRETCH" : "LIVE", status, juce::Justification::centredLeft);
            g.setColour(mint.withAlpha(.85f * alpha));
            g.drawText(juce::String(count) + "/" + juce::String(juce::roundToInt(voices->load(std::memory_order_relaxed)))
                           + " GRAINS", status, juce::Justification::centredRight);
        }
        retro::crtGlass(g, area, 5);
    }

private:
    struct Spark
    {
        juce::Point<float> at, velocity;
        juce::Colour colour;
        float life = 0, age = 0, size = 1;
        bool ring = false;
    };
    struct Mote
    {
        float x, y, depth, phase;
    };

    juce::Rectangle<float> graphBounds() const
    {
        return getLocalBounds().toFloat().reduced(.5f).reduced(9, 0).withTrimmedTop(17).withTrimmedBottom(7);
    }

    // Distances behind the input sit where the Position knob would put them.
    float xFor(float ms) const
    {
        const auto graph = graphBounds();
        const auto& range = position->getNormalisableRange();
        return graph.getRight() - range.convertTo0to1(range.snapToLegalValue(ms)) * graph.getWidth();
    }

    float pitchY(float semitones) const
    {
        const auto graph = graphBounds();
        // The outer octaves stop short of the edges, so the highest and lowest grains stay clear of the readouts.
        return graph.getCentreY() - juce::jlimit(-1.f, 1.f, semitones / 24.f) * graph.getHeight() * .42f;
    }

    // A pad with nothing patched from X or Y would do nothing; the first drag gives it the Grain card's own axes.
    void patchIfUnused()
    {
        for (const auto* axis : { "xy_x_target_", "xy_y_target_" })
            for (int slot = 1; slot <= 4; ++slot)
                if (state.getRawParameterValue(axis + juce::String(slot))->load() >= .5f)
                    return;
        const auto write = [this](const juce::String& id, float value)
        {
            if (auto* p = state.getParameter(id))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(p->convertTo0to1(value));
                p->endChangeGesture();
            }
        };
        write("xy_x_target_1", static_cast<float>(grainPositionTarget));
        write("xy_x_depth_1", -60);
        write("xy_x_active_1", 1);
        write("xy_y_target_1", static_cast<float>(grainPitchTarget));
        write("xy_y_depth_1", 50);
        write("xy_y_active_1", 1);
    }

    // Where the pad's handle sits: the X and Y macros across the whole screen, the middle changing nothing.
    juce::Point<float> handle() const
    {
        const auto graph = graphBounds();
        return { graph.getX() + padX->getValue() * graph.getWidth(), graph.getBottom() - padY->getValue() * graph.getHeight() };
    }

    void moveTo(juce::Point<float> point)
    {
        const auto graph = graphBounds();
        padX->setValueNotifyingHost(juce::jlimit(0.f, 1.f, (point.x - graph.getX()) / graph.getWidth()));
        padY->setValueNotifyingHost(juce::jlimit(0.f, 1.f, (graph.getBottom() - point.y) / graph.getHeight()));
        repaint();
    }

    void burst(juce::Point<float> at, juce::Colour colour)
    {
        add({ at, {}, colour, .4f, 0, 3.f, true });
        for (int k = 0; k < 5; ++k)
        {
            const auto angle = random.nextFloat() * juce::MathConstants<float>::twoPi;
            const auto speed = 18.f + 22.f * random.nextFloat();
            add({ at, { std::cos(angle) * speed, std::sin(angle) * speed }, colour, .35f + .3f * random.nextFloat(), 0,
                  .9f + .6f * random.nextFloat(), false });
        }
    }

    // A sounding grain sheds sparks: more as its window swells, rising for grains pitched up and sinking for
    // grains pitched down.
    void shed(juce::Point<float> at, juce::Colour colour, float swell, float semitones, float dt)
    {
        auto expected = dt * 26.f * swell;
        while (expected > 0)
        {
            if (random.nextFloat() < expected)
            {
                const auto lift = -juce::jlimit(-1.f, 1.f, semitones / 12.f) * 10.f;
                add({ at + juce::Point<float>(random.nextFloat() * 4.f - 2.f, random.nextFloat() * 4.f - 2.f),
                      { -6.f - 14.f * random.nextFloat(), lift + 10.f * (random.nextFloat() - .5f) },
                      colour, .5f + .6f * random.nextFloat(), 0, .6f + .9f * random.nextFloat() * swell, false });
            }
            expected -= 1.f;
        }
    }

    void add(Spark spark)
    {
        if (sparks.size() < 260)
            sparks.push_back(spark);
    }

    void paintGrain(juce::Graphics& g, juce::Point<float> at, juce::Colour colour, float swell) const
    {
        const auto radius = 1.6f + 3.2f * swell;
        for (const auto& [spread, glow] : { std::pair { 3.2f, .1f }, std::pair { 1.8f, .22f } })
        {
            g.setColour(colour.withAlpha(glow * (.4f + .6f * swell)));
            g.fillEllipse(juce::Rectangle<float>(2 * radius * spread, 2 * radius * spread).withCentre(at));
        }
        g.setColour(colour.withAlpha(.45f + .55f * swell));
        g.fillEllipse(juce::Rectangle<float>(2 * radius, 2 * radius).withCentre(at));
        g.setColour(juce::Colours::white.withAlpha(.5f * swell));
        g.fillEllipse(juce::Rectangle<float>(radius * .8f, radius * .8f).withCentre(at));
    }

    void paintSparks(juce::Graphics& g, float dt)
    {
        for (auto& spark : sparks)
        {
            spark.age += dt;
            spark.at += spark.velocity * dt;
            spark.velocity *= 1.f - juce::jmin(1.f, 1.6f * dt);
        }
        sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& s) { return s.age >= s.life; }),
                     sparks.end());
        for (const auto& spark : sparks)
        {
            const auto fade = 1.f - spark.age / spark.life;
            if (spark.ring)
            {
                const auto radius = spark.size + 9.f * (1.f - fade * fade);
                g.setColour(spark.colour.withAlpha(.4f * fade));
                g.drawEllipse(juce::Rectangle<float>(2 * radius, 2 * radius).withCentre(spark.at), 1.f);
                continue;
            }
            const auto size = spark.size * (.5f + .5f * fade);
            g.setColour(spark.colour.withAlpha(.18f * fade));
            g.fillEllipse(juce::Rectangle<float>(size * 4.f, size * 4.f).withCentre(spark.at));
            g.setColour(spark.colour.withAlpha(.85f * fade));
            g.fillEllipse(juce::Rectangle<float>(size * 1.6f, size * 1.6f).withCentre(spark.at));
        }
    }

    // Faint dust flowing from the input on the right into the past on the left; it slows while stretching.
    void paintDust(juce::Graphics& g, juce::Rectangle<float> graph, float dt, bool on, bool stretching)
    {
        const auto speed = on ? (stretching ? .015f + .06f * stretchSpeed->load(std::memory_order_relaxed) * .01f : .075f) : .01f;
        for (auto& mote : dust)
        {
            mote.x += dt * speed * (.5f + mote.depth);
            mote.phase += dt * (.6f + mote.depth);
            if (mote.x > 1.f)
            {
                mote.x -= 1.f;
                mote.y = random.nextFloat();
            }
            const juce::Point<float> at { graph.getRight() - mote.x * graph.getWidth(),
                                          graph.getY() + mote.y * graph.getHeight() + 1.5f * std::sin(mote.phase * 2.f) };
            const auto twinkle = .5f + .5f * std::sin(mote.phase * 5.f);
            g.setColour(mint.withAlpha((on ? .05f + .12f * twinkle : .04f) * mote.depth));
            const auto size = .7f + 1.1f * mote.depth;
            g.fillEllipse(juce::Rectangle<float>(size, size).withCentre(at));
        }
    }

    // The pad's handle: a ring on faint crosshairs, bright while it is held.
    void paintHandle(juce::Graphics& g, juce::Rectangle<float> graph, float alpha) const
    {
        const auto at = handle();
        const auto strength = (dragging ? 1.f : hovered ? .7f : .4f) * alpha;
        g.setColour(gold.withAlpha(.16f * strength));
        g.drawVerticalLine(juce::roundToInt(at.x), graph.getY(), graph.getBottom());
        g.drawHorizontalLine(juce::roundToInt(at.y), graph.getX(), graph.getRight());
        const auto radius = dragging ? 6.5f : 5.f;
        g.setColour(gold.withAlpha(.25f * strength));
        g.drawEllipse(juce::Rectangle<float>(2 * radius + 4, 2 * radius + 4).withCentre(at), 2.f);
        g.setColour(gold.withAlpha(.9f * strength));
        g.drawEllipse(juce::Rectangle<float>(2 * radius, 2 * radius).withCentre(at), 1.3f);
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered = true; }
    void mouseExit(const juce::MouseEvent&) override { hovered = false; }

    void tick()
    {
        const auto on = active->load(std::memory_order_relaxed) >= .5f;
        if (on || wasOn || !sparks.empty())
            repaint();
        wasOn = on;
    }

    const juce::Colour mint { 0xff8ff5c4 }, gold { 0xffe6c15a };
    const std::atomic<float>* active;
    const std::atomic<float>* stretch;
    const std::atomic<float>* voices;
    const std::atomic<float>* stretchSpeed;
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter* position;
    juce::RangedAudioParameter* padX;
    juce::RangedAudioParameter* padY;
    std::vector<Spark> sparks;
    std::array<Mote, 48> dust {};
    std::array<bool, GranularCloud::voiceCount> seen {};
    std::array<float, GranularCloud::voiceCount> progress {};
    juce::Random random;
    double last = 0;
    bool dragging = false, hovered = false, wasOn = true;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// The cassette in the Tape card's window. Its spools turn while audio plays, swaying with the wow, slowing with a
// Stretch further down the chain and spinning up or down like a motor; a hard-worked tape shows tracking trouble.
// It can be handled: pressing on it holds the tape still, dragging sideways winds it on or pulls it back, and
// letting go hands it back to the motor.
class TapeReels final : public juce::Component
{
public:
    TapeReels()
    {
        setComponentID("tape_reels");
        setTitle("Tape reels: press to hold the tape, drag sideways to scrub");
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }

    // Reports a hand on the reels: engaged, and the tape speed it asks for.
    std::function<void(bool engaged, float speed)> onScrub;

    void mouseDown(const juce::MouseEvent& event) override
    {
        handled = true;
        handSpeed = 0;
        lastX = event.position.x;
        lastMove = juce::Time::getMillisecondCounterHiRes();
        if (onScrub)
            onScrub(true, 0);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto seconds = juce::jmax(.004, (now - lastMove) * .001);
        // About 120 pixels a second of hand movement is normal play speed; faster drags wind faster.
        const auto measured = static_cast<float>((event.position.x - lastX) / seconds / 120.0);
        handSpeed += (juce::jlimit(-4.f, 4.f, measured) - handSpeed) * .5f;
        lastX = event.position.x;
        lastMove = now;
        if (onScrub)
            onScrub(true, handSpeed);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        handled = false;
        handSpeed = 0;
        if (onScrub)
            onScrub(false, 0);
    }

    // `speed` is 1 at normal play, less while a Stretch slows the audio and 0 when stopped. `wowDepth` (0 to 1)
    // and `wowRate` (Hz) sway it; `strain` (0 to 1) brings on tracking noise.
    void setTransport(float speed, float wowDepth, float wowRate, float strain)
    {
        target = speed;
        wow = wowDepth;
        rate = wowRate;
        noise = strain;
    }

    void paint(juce::Graphics& g) override
    {
        const auto* card = getParentComponent();
        if (card == nullptr)
            return;
        g.addTransform(juce::AffineTransform::translation(static_cast<float>(-getX()), static_cast<float>(-getY())));
        retro::tapeWindow(g, card->getLocalBounds(), left, right);
        retro::trackingNoise(g, retro::tapeWindowBounds(card->getLocalBounds()).reduced(1.f), 6,
                             noise * juce::jmin(1.f, std::abs(speed) * 2.f), juce::Time::getMillisecondCounterHiRes() * .001, seed);
    }

private:
    void tick()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        const auto dt = static_cast<float>(juce::jlimit(0.0, .1, now - last));
        last = now;
        // A hand that stops moving holds the tape still.
        if (handled && now * 1000.0 - lastMove > 90.0 && handSpeed != 0)
        {
            handSpeed *= .5f;
            if (std::abs(handSpeed) < .02f)
                handSpeed = 0;
            if (onScrub)
                onScrub(true, handSpeed);
        }
        // The capstan motor takes a moment to reach speed and coasts to a stop; a hand moves the reels directly.
        const auto goal = handled ? handSpeed : target;
        const auto lag = handled ? .04f : std::abs(goal) > std::abs(speed) ? .3f : .45f;
        speed += (goal - speed) * (1.f - std::exp(-dt / lag));
        if (std::abs(speed) < .002f && std::abs(goal) < .002f)
        {
            if (speed != 0)
            {
                speed = 0;
                repaint();
            }
            return;
        }
        const auto sway = 1.f + .12f * wow * std::sin(static_cast<float>(now) * rate * juce::MathConstants<float>::twoPi);
        // About half a turn a second; the fuller supply reel turns more slowly than the take-up reel.
        const auto turn = speed * sway * dt * juce::MathConstants<float>::twoPi * .55f;
        left = std::fmod(left + turn * .8f, juce::MathConstants<float>::twoPi);
        right = std::fmod(right + turn * 1.2f, juce::MathConstants<float>::twoPi);
        repaint();
    }

    float target = 0, speed = 0, wow = 0, rate = 1, noise = 0, left = 0, right = .7f, handSpeed = 0, lastX = 0;
    double last = 0, lastMove = 0;
    bool handled = false;
    const juce::uint32 seed = static_cast<juce::uint32>(juce::Random::getSystemRandom().nextInt());
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

class CartridgeDrawer final : public juce::Component
{
public:
    CartridgeDrawer(const CartridgeImages& images, juce::AudioProcessorValueTreeState& state, int instance)
        : art(images), type(static_cast<size_t>(instance) % modules.size()),
          active(state.getRawParameterValue(effectPrefix(instance) + "active"))
    {
        const auto prefix = effectPrefix(instance);
        // The cartridge label already names the effect, so the panel carries no title. Its power lamp and the
        // key to the full controls are mounted on the dock bar by the slot (see CartridgeSlot).
        setTitle(modules[type].name);
        styleButton(power, ButtonFace::lamp);
        power.setTitle(juce::String("Toggle ") + modules[type].name);
        power.setComponentID(prefix + "power");
        styleButton(details, ButtonFace::controls);
        details.setTitle(juce::String("All ") + modules[type].name + " controls");
        details.setComponentID(prefix + "controls");
        if (prefix.endsWith("stretch_"))
        {
            strip.setComponentID(prefix + "map_screen");
            strip.attach(state.getParameter(prefix + "start"), state.getParameter(prefix + "land"));
            addAndMakeVisible(strip);
        }
        if (prefix.endsWith("vhs_"))
        {
            vhsScreen = std::make_unique<VhsScreen>(state, prefix);
            addAndMakeVisible(*vhsScreen);
        }
        if (prefix.endsWith("grain_"))
        {
            grainScreen = std::make_unique<GrainScreen>(state, prefix);
            addAndMakeVisible(*grainScreen);
        }
        if (prefix.endsWith("tape_"))
        {
            for (const auto* id : { "depth", "wow_rate", "flutter_amount", "dropout_depth" })
                tapeValues.push_back(state.getRawParameterValue(prefix + id));
            addAndMakeVisible(reels);
            reels.onScrub = [this](bool engaged, float speed)
            {
                if (onScrub)
                    onScrub(engaged, speed);
            };
        }
        if (prefix.endsWith("delay_"))
        {
            note = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter(prefix + "note"));
            timing = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter(prefix + "timing"));
            sync = dynamic_cast<juce::AudioParameterBool*>(state.getParameter(prefix + "sync"));
            styleButton(division, ButtonFace::selector);
            division.setTitle("Delay tempo division");
            division.setComponentID(prefix + "division");
            division.onClick = [this] { showDivisions(); };
            addAndMakeVisible(division);
        }
        else
        {
            const auto* quickParameter = prefix.endsWith("reverb_") ? "type" : prefix.endsWith("stretch_") ? "engine"
                : prefix.endsWith("vhs_") ? "speed" : prefix.endsWith("grain_") ? "scale"
                : prefix.endsWith("chorus_") ? "stereo" : prefix.endsWith("tape_") ? "noise_type" : nullptr;
            if (quickParameter != nullptr)
            {
                const auto id = prefix + quickParameter;
                auto* parameter = state.getParameter(id);
                if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(parameter))
                    mode.addItemList(choice->choices, 1);
                else if (dynamic_cast<juce::AudioParameterBool*>(parameter) != nullptr)
                {
                    mode.addItem("Mono", 1);
                    mode.addItem("Stereo", 2);
                }

                if (mode.getNumItems() > 0)
                {
                    mode.setComponentID(prefix + "quick_mode");
                    mode.getProperties().set("cassette_header_text", true);
                    mode.setTitle(parameter->getName(100));
                    mode.setJustificationType(juce::Justification::centred);
                    mode.setMouseClickGrabsKeyboardFocus(false);
                    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
                        state, id, mode);
                    addAndMakeVisible(mode);
                }
            }
        }

        for (const auto* id : featured[type].knobs)
        {
            if (id == nullptr)
                continue;
            if (auto* parameter = state.getParameter(prefix + id))
            {
                auto tile = std::make_unique<ParameterTile>(state, *parameter, controlName(*parameter, prefix),
                                                            type, CartridgeImages::panelText(type), true);
                tile->leaveShadowToParent();
                addAndMakeVisible(*tile);
                knobs.push_back(std::move(tile));
            }
        }
    }

    void paint(juce::Graphics& g) override
    {
        // Once every knob rests on the panel their shadows are part of the cached panel; while they rise in or
        // drop, the bare panel is cached and the shadows follow them live.
        const auto settled = std::all_of(knobs.begin(), knobs.end(), [](const auto& tile)
        {
            return tile->getAlpha() >= .999f && tile->drop() <= 0.f;
        });
        panel[settled ? 1 : 0].draw(g, getLocalBounds(), [this, settled](juce::Graphics& face)
        {
            art.drawPanel(face, getLocalBounds(), type);
            // The dock above casts a shadow onto the top of the panel.
            face.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(.55f), 0, 0,
                                                      juce::Colours::transparentBlack, 0, 16, false));
            face.fillRect(getLocalBounds().removeFromTop(16).reduced(2, 0));
            if (settled)
                paintKnobShadows(face);
        });
        if (!settled)
            paintKnobShadows(g);
    }

    void paintKnobShadows(juce::Graphics& g)
    {

        // Knob shadows, painted here so the narrow tiles don't cut them off.
        for (auto& tile : knobs)
        {
            const auto local = tile->knob();
            const auto alpha = tile->getAlpha();
            if (local.isEmpty() || alpha <= .01f)
                continue;
            const auto area = getLocalArea(tile.get(), local);
            const bool fading = alpha < .99f;
            if (fading)
                g.beginTransparencyLayer(alpha);
            droppedKnobShadow(g, area.getCentre(), area.getWidth(), tile->drop());
            if (fading)
                g.endTransparencyLayer();
        }
    }

    void refresh()
    {
        const auto engaged = active->load(std::memory_order_relaxed) >= .5f;
        power.setToggleState(engaged, juce::dontSendNotification);
        if (note != nullptr)
            division.setButtonText(sync->get() ? divisionName(note->getIndex(), timing->getIndex()) : "Free");
        if (stretchSource)
            strip.update(stretchSource());
    }

    // Supplies the live Stretch program for the screen on a Stretch card.
    std::function<StretchPreview()> stretchSource;

    // Supplies the live grains for the screen on a Grain card.
    void setGrainSource(std::function<GrainPicture()> source)
    {
        if (grainScreen)
            grainScreen->source = std::move(source);
    }

    // A hand on this card's reels; the surface passes it to the processor.
    std::function<void(bool engaged, float speed)> onScrub;

    // How fast the tape runs: 1 while audio plays, less while a Stretch slows it, 0 in silence.
    void setTransport(float speed)
    {
        if (vhsScreen)
            vhsScreen->setTransport(speed);
        if (tapeValues.size() < 4)
            return;
        const auto read = [this](size_t i) { return tapeValues[i]->load(std::memory_order_relaxed); };
        const auto on = active->load(std::memory_order_relaxed) >= .5f;
        const auto depth = read(0) * .01f, flutter = read(2) * .01f, dropouts = read(3) * .01f;
        // A deep, fluttering, dropping-out tape is working hard enough to show it on the picture.
        const auto strain = juce::jlimit(0.f, .8f, (depth + flutter + dropouts) * .5f - .35f);
        reels.setTransport(on ? speed : 0.f, depth, read(1), on ? strain : 0.f);
    }

    void resized() override
    {
        const auto dividerY = juce::roundToInt(getHeight() * compactDividerPosition);
        // Cards with a quick selector keep a header strip for it, centred; the cartridge covers the first
        // few pixels of the face.
        const auto heading = getLocalBounds().withTrimmedTop(drawerOverlap).withBottom(dividerY).reduced(12, 0);
        const auto selectorBounds = heading.withSizeKeepingCentre(132, 24);
        if (note != nullptr)
            division.setBounds(selectorBounds);
        if (modeAttachment != nullptr)
            mode.setBounds(selectorBounds);

        // Each card lays out its controls its own way, with a large hero knob on most of them.
        const auto layout = retro::cardLayout(type);
        for (size_t i = 0; i < knobs.size() && i < layout.size(); ++i)
        {
            knobs[i]->setHero(layout[i].hero);
            knobs[i]->setBounds(layout[i].bounds);
        }

        // The Stretch card gives the upper half of its face to the map screen.
        strip.setBounds(retro::cardScreen(type));
        if (vhsScreen)
            vhsScreen->setBounds(retro::cardScreen(type));
        if (grainScreen)
            grainScreen->setBounds(retro::cardScreen(type));
        if (!tapeValues.empty())
            reels.setBounds(retro::tapeWindowBounds(getLocalBounds()).getSmallestIntegerContainer());
    }

    juce::TextButton power, details { "Controls" };

    // Power-on sequence, played once the drawer has dropped into place: the lamp stutters before it holds,
    // the controls rise in one after another and every knob sweeps to its end stop before settling.
    void prepareBoot()
    {
        for (auto& tile : knobs)
        {
            tile->setAlpha(0.f);
            tile->setSweep(0.f);
            tile->setDrop(1.f);
        }
        power.getProperties().set("cassette_dark", true);
        power.repaint();
    }

    void startBoot(double now) { bootStart = now; }

    // Advances the sequence; returns false once it has finished.
    bool tickBoot(double now)
    {
        if (bootStart < 0)
            return false;
        const auto t = static_cast<float>((now - bootStart) * .001);

        static constexpr std::array<float, 4> flicker { .05f, .09f, .14f, .2f };
        const auto edges = std::count_if(flicker.begin(), flicker.end(), [t](float edge) { return t >= edge; });
        const auto dark = edges % 2 == 1;
        if (static_cast<bool>(power.getProperties()["cassette_dark"]) != dark)
        {
            power.getProperties().set("cassette_dark", dark);
            power.repaint();
        }

        constexpr float stagger = .07f, rise = .34f, sweep = .75f;
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const auto delay = stagger * static_cast<float>(i);
            const auto appear = juce::jlimit(0.f, 1.f, (t - .12f - delay) / rise);
            knobs[i]->setAlpha(juce::jmin(1.f, appear * 3.f));
            knobs[i]->setDrop(appear > 0 ? dropHeight(appear) : 1.f);
            const auto swing = (t - .2f - delay) / sweep;
            knobs[i]->setSweep(swing >= 1.f ? -1.f : juce::jmax(0.f, swing));
        }

        repaint(); // The knob shadows painted by this panel follow the rising, fading tiles.
        if (t > .2f + stagger * static_cast<float>(knobs.size()) + sweep + .05f)
        {
            finishBoot();
            return false;
        }
        return true;
    }

    void finishBoot()
    {
        bootStart = -1;
        for (auto& tile : knobs)
        {
            tile->setAlpha(1.f);
            tile->setTransform({});
            tile->setDrop(0.f);
            tile->setSweep(-1.f);
        }
        power.getProperties().remove("cassette_dark");
        power.repaint();
        repaint();
    }

private:
    double bootStart = -1;

    juce::String divisionName(int n, int feel) const
    {
        return note->choices[n] + (feel == 0 ? juce::String{} : " " + timing->choices[feel]);
    }

    void showDivisions()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Free time", true, !sync->get());
        for (int feel = 0; feel < timing->choices.size(); ++feel)
        {
            juce::PopupMenu divisions;
            for (int n = 0; n < note->choices.size(); ++n)
                divisions.addItem(2 + feel * note->choices.size() + n, divisionName(n, feel), true,
                    sync->get() && feel == timing->getIndex() && n == note->getIndex());
            menu.addSubMenu(timing->choices[feel], divisions);
        }
        menu.setLookAndFeel(&getLookAndFeel());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(division),
            [safe = juce::Component::SafePointer<CartridgeDrawer>(this)](int id)
            {
                if (safe == nullptr || id == 0)
                    return;
                const auto write = [](juce::RangedAudioParameter& parameter, float value)
                {
                    parameter.beginChangeGesture();
                    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
                    parameter.endChangeGesture();
                };
                if (id > 1)
                {
                    const auto count = safe->note->choices.size();
                    write(*safe->note, static_cast<float>((id - 2) % count));
                    write(*safe->timing, static_cast<float>((id - 2) / count));
                }
                write(*safe->sync, id > 1 ? 1.f : 0.f);
                safe->refresh();
            });
    }

    const CartridgeImages& art;
    const size_t type;
    const std::atomic<float>* active;
    juce::AudioParameterChoice* note = nullptr;
    juce::AudioParameterChoice* timing = nullptr;
    juce::AudioParameterBool* sync = nullptr;
    juce::TextButton division;
    juce::ComboBox mode;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;
    std::vector<std::unique_ptr<ParameterTile>> knobs;
    StretchMap strip { true };
    TapeReels reels;
    std::unique_ptr<VhsScreen> vhsScreen;
    std::unique_ptr<GrainScreen> grainScreen;
    std::vector<const std::atomic<float>*> tapeValues;
    std::array<PaintCache, 2> panel;
};

class CartridgeSlot final : public juce::Component
{
public:
    CartridgeSlot(const CartridgeImages& images, juce::AudioProcessorValueTreeState& state,
                  int instanceValue, int index, bool animate)
        : art(images), instance(instanceValue)
    {
        insert.setButtonText(instance < 0 ? "Empty cartridge slot" : modules[instance % modules.size()].name);
        insert.setComponentID("slot_" + juce::String(index));
        insert.setTitle(instance < 0 ? "Drop a cartridge here" : "Drag to change signal order");
        insert.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        styleButton(eject, ButtonFace::close);
        eject.setComponentID("eject_" + juce::String(index));
        eject.setTitle("Remove slot " + juce::String(index + 1));
        insert.artwork = [this](auto& g, auto bounds, bool hovered) { drawSlot(g, bounds, hovered); };
        addAndMakeVisible(insert);
        addAndMakeVisible(eject);

        if (instance >= 0)
        {
            drawer = std::make_unique<CartridgeDrawer>(art, state, instance);
            drawerWindow.addAndMakeVisible(*drawer);
            addAndMakeVisible(drawerWindow);
            drawerWindow.toBack();
            // The cartridge's power lamp and the key to its full controls sit on the dock bar, beside eject.
            addAndMakeVisible(drawer->power);
            addAndMakeVisible(drawer->details);
            drawer->refresh();
            if (animate)
            {
                // Starts tucked up inside the dock, then drops out.
                offset = static_cast<float>(drawerHeight);
                dropping = true;
                drawer->prepareBoot();
            }
        }
        else if (animate)
        {
            // A new empty dock falls into the rack from above, fading in as it comes.
            dockDrop = dockFall;
            dockFalling = true;
            placeDock();
        }
    }

    void resized() override
    {
        insert.setBounds(0, 0, getWidth(), slotHeight);
        const Area ejectKey(getWidth() - 30, 8, 17, 17);
        eject.setBounds(ejectKey);
        if (drawer)
        {
            drawer->power.setBounds(Area(16, 16).withCentre({ 16, ejectKey.getCentreY() }));
            drawer->details.setBounds(ejectKey.translated(-23, 0));
        }
        drawerWindow.setBounds(4, slotHeight - drawerOverlap, getWidth() - 8, drawerHeight);
        placeDrawer();
    }

    void refresh()
    {
        // Without a window on screen no frames arrive, so show the finished state straight away.
        if (getPeer() == nullptr && (dropping || booting || dockFalling || awaiting || landedAt > 0))
        {
            offset = dockDrop = 0;
            dropping = booting = dockFalling = awaiting = false;
            landedAt = sparkStart = 0;
            placeDrawer();
            placeDock();
            if (drawer)
                drawer->finishBoot();
        }
        if (drawer)
            drawer->refresh();
    }

    void highlight(bool value)
    {
        if (std::exchange(target, value) != value)
            insert.repaint();
    }

    // Takes the slot out of the rack: the drawer retracts into the dock as its lamp goes dark, then the dock
    // lifts away and fades. `done` runs afterwards, once; without a window on screen it runs straight away.
    void remove(std::function<void()> done)
    {
        if (leaving)
            return;
        if (getPeer() == nullptr)
        {
            done();
            return;
        }
        onRemoved = std::move(done);
        leaving = true;
        leaveStart = juce::Time::getMillisecondCounterHiRes();
        dropping = booting = dockFalling = false;
        landedAt = sparkStart = 0;
        setInterceptsMouseClicks(false, false);
        if (drawer)
        {
            drawer->power.getProperties().set("cassette_dark", true);
            drawer->power.repaint();
        }
    }

    // A slot torn down mid-exit (the rack was rebuilt, or the editor closed) still finishes the eject it started,
    // so its effect is never left faded out in the rack.
    ~CartridgeSlot() override
    {
        if (auto done = std::exchange(onRemoved, nullptr))
            done();
    }

    bool isLeaving() const { return leaving; }

    // For Generate's deal: the dock falls into the rack empty and waits for its cartridge to be thrown in.
    void awaitCartridge()
    {
        if (drawer == nullptr || getPeer() == nullptr)
            return;
        awaiting = true;
        offset = static_cast<float>(drawerHeight);
        placeDrawer();
        drawer->prepareBoot();
        dockDrop = dockFall;
        dockFalling = true;
        placeDock();
        insert.repaint();
    }

    bool isAwaitingCartridge() const { return awaiting; }

    // An empty dock falls into the rack from above, fading in as it comes.
    void fallIn()
    {
        if (getPeer() == nullptr)
            return;
        dockDrop = dockFall;
        dockFalling = true;
        placeDock();
    }

    // The thrown cartridge has landed in the dock: it seats with a knock and its drawer drops out.
    void seat()
    {
        if (!std::exchange(awaiting, false))
            return;
        insert.repaint();
        speed = 0;
        dropping = true;
        jolt(.7f);
    }

    // The power-up charge: how far the arcs have run (0 to 1), how bright they are, and this frame's number.
    struct Spark
    {
        float reach = 0, intensity = 0;
        juce::uint32 frame = 0;
        juce::Rectangle<float> panel;
    };

    std::optional<Spark> spark() const
    {
        if (sparkStart <= 0 || drawer == nullptr)
            return std::nullopt;
        const auto t = static_cast<float>((juce::Time::getMillisecondCounterHiRes() - sparkStart) * .001);
        constexpr float travel = .1f, hold = .18f, fade = .22f;
        Spark result;
        result.reach = juce::jlimit(0.f, 1.f, t / travel);
        result.intensity = t < hold ? 1.f : juce::jmax(0.f, 1.f - (t - hold) / fade);
        result.frame = static_cast<juce::uint32>(t * 30.f);
        // A flicker that changes with each frame of the crackle.
        result.intensity *= .78f + .22f * juce::Random(static_cast<juce::int64>(result.frame) * 31 + instance).nextFloat();
        const auto panel = drawer->getBounds().getIntersection(drawerWindow.getLocalBounds()) + drawerWindow.getPosition();
        result.panel = panel.toFloat().reduced(1.5f);
        return result;
    }

    // The charge in the rack's coordinates; the rack draws the arcs and their light above every card
    // (see RackSparks), so neighbouring cards never cut them off.
    std::optional<Spark> sparkInRack() const
    {
        auto charge = spark();
        if (charge)
        {
            charge->panel += getPosition().toFloat().translated(slideX, 0);
            charge->frame += static_cast<juce::uint32>(instance) * 977u;
        }
        return charge;
    }

    // Glides in from `dx` pixels away, after the slots around it have changed.
    void slideFrom(float dx)
    {
        if (getPeer() == nullptr || std::abs(dx) < .5f)
            return;
        slideFromX = dx;
        slideStart = juce::Time::getMillisecondCounterHiRes();
        sliding = true;
        placeSlide();
    }

    // Called the moment a falling drawer or dock hits its stop, with how hard it landed (0 to 1).
    std::function<void(float strength)> onLanded;

    // A knock passed through the rack from a neighbour landing: after `delay` seconds the slot jumps down and
    // rings out like a sprung part.
    void jolt(float strength, double delay = 0)
    {
        if (getPeer() == nullptr || strength <= .01f)
            return;
        joltStart = juce::Time::getMillisecondCounterHiRes() + delay * 1000.0;
        joltStrength = juce::jmax(joltStrength * joltEnvelope(juce::Time::getMillisecondCounterHiRes()), strength);
        jolting = true;
    }

    // Outlines of the dock and the extended drawer, for the shadow the slot casts onto the rack.
    std::array<Area, 2> silhouette() const
    {
        const auto visible = drawer != nullptr
            ? drawer->getBounds().getIntersection(drawerWindow.getLocalBounds()) + drawerWindow.getPosition()
            : Area();
        // A falling dock's shadow follows it down and firms up as it fades in.
        const auto dock = dockDrop < dockFall * .7f ? Area(0, -juce::roundToInt(dockDrop), getWidth(), slotHeight).reduced(3, 2)
                                                    : Area();
        // A sliding or jolted slot carries its shadow with it.
        const auto at = getPosition().translated(juce::roundToInt(slideX), juce::roundToInt(joltY));
        return { dock.isEmpty() ? Area() : dock + at,
                 visible.isEmpty() ? Area() : visible.reduced(2, 0) + at };
    }

    ArtworkButton insert;
    juce::TextButton eject { juce::String::charToString(0x00d7) };
    std::unique_ptr<CartridgeDrawer> drawer;
    const int instance;

private:
    void drawSlot(juce::Graphics& g, Area bounds, bool hover)
    {
        const auto port = Area(116, 20).withCentre({ bounds.getCentreX(), bounds.getBottom() - 22 });
        if (instance >= 0 && !awaiting)
        {
            // A seated cartridge only changes when hovered, so both looks are kept as images.
            dockArt[hover ? 1 : 0].draw(g, bounds, [this, bounds, hover](juce::Graphics& dock)
            {
                art.draw(dock, CartridgeImages::slot, bounds);
                const auto cavity = bounds.reduced(cavityInset, 0).withTrimmedTop(cavityTop).withTrimmedBottom(cavityBottom);
                art.drawCartridge(dock, cavity, instance % modules.size(), hover);
                art.drawSeatShadow(dock, cavity);
                art.drawConnector(dock, Area(cavity.getWidth(), 19).withCentre(
                    { bounds.getCentreX(), bounds.getBottom() - 10 }));
            });
        }
        else
        {
            art.draw(g, CartridgeImages::slot, bounds);
            // Once a new dock lands its contacts flash and the hint fades up.
            const auto since = landedAt > 0 ? static_cast<float>((juce::Time::getMillisecondCounterHiRes() - landedAt) * .001) : -1.f;
            const auto flash = since >= 0 ? juce::jmax(0.f, 1.f - since / .7f) : 0.f;
            const auto hint = dockFalling && since < 0 ? 0.f : since >= 0 ? juce::jmin(1.f, since / .35f) : 1.f;
            if (flash > 0)
            {
                g.setColour(accent.withAlpha(.35f * flash));
                g.fillRoundedRectangle(port.toFloat().expanded(8, 5), 9);
            }
            art.drawDock(g, port, false);
            if (flash > 0)
            {
                g.setColour(juce::Colour(0xffffe3a0).withAlpha(.45f * flash));
                g.fillRoundedRectangle(port.toFloat().reduced(port.getWidth() * .2f, 6), 3);
            }
            if (instance < 0)
            {
                g.setColour(cream.withMultipliedAlpha(hint));
                g.setFont(font(15));
                g.drawText("Drag a cartridge here", bounds.reduced(16).translated(0, -9), juce::Justification::centred);
            }
        }
        if (target)
        {
            g.setColour(accent);
            g.drawRoundedRectangle(bounds.toFloat().reduced(2), 10, 2);
        }
    }

    void placeDrawer()
    {
        if (drawer)
            drawer->setBounds(0, -juce::roundToInt(offset), drawerWindow.getWidth(), drawerHeight);
    }

    void placeDock()
    {
        const auto lift = juce::AffineTransform::translation(0, -dockDrop);
        const auto opacity = juce::jlimit(0.f, 1.f, 1.f - dockDrop / dockFall);
        std::vector<juce::Component*> parts { &insert, &eject };
        if (drawer)
        {
            parts.push_back(&drawer->power);
            parts.push_back(&drawer->details);
        }
        for (auto* part : parts)
        {
            part->setTransform(lift);
            part->setAlpha(opacity);
        }
        repaintTray();
    }

    void placeSlide()
    {
        setTransform(juce::AffineTransform::translation(slideX, joltY));
        repaintTray();
    }

    // How much of a jolt is left at time `now` (ms), from 1 at the knock down to 0.
    float joltEnvelope(double now) const
    {
        if (!jolting || now < joltStart)
            return jolting ? 1.f : 0.f;
        return std::exp(-static_cast<float>((now - joltStart) * .001) * 11.f);
    }

    void tickJolt(double now)
    {
        if (!jolting || now < joltStart)
            return;
        const auto t = static_cast<float>((now - joltStart) * .001);
        const auto envelope = std::exp(-t * 11.f);
        // Down first, then a quick damped ring, as a sprung tray does when something lands beside it.
        joltY = 3.2f * joltStrength * envelope * std::sin(t * juce::MathConstants<float>::twoPi * 13.f);
        if (envelope < .02f)
        {
            jolting = false;
            joltY = 0;
            joltStrength = 0;
        }
        placeSlide();
    }

    // The shadow on the rack floor follows the dock and drawer.
    void repaintTray()
    {
        // A charging cartridge's bloom reaches much further than its shadow.
        const auto reach = sparkStart > 0 ? 220 : 40;
        if (auto* parent = getParentComponent())
            parent->repaint(getBounds().expanded(reach + juce::roundToInt(std::abs(slideX)), sparkStart > 0 ? 220 : 30));
    }

    // Leaving: the drawer rises back into the dock (accelerating, like it is drawn in), then the dock lifts off.
    void tickLeave(double now)
    {
        const auto t = static_cast<float>((now - leaveStart) * .001);
        const auto retract = drawer != nullptr ? .24f : 0.f;
        if (drawer)
        {
            const auto u = juce::jlimit(0.f, 1.f, t / retract);
            offset = static_cast<float>(drawerHeight) * u * u;
            placeDrawer();
        }
        const auto u = juce::jlimit(0.f, 1.f, (t - retract) / .26f);
        dockDrop = dockFall * u * u;
        placeDock();
        if (u >= 1.f)
        {
            leaving = false;
            setVisible(false);
            repaintTray();
            if (onRemoved)
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CartridgeSlot>(this)]
                {
                    // If the slot is gone by now, its destructor has already run the completion.
                    if (safe != nullptr)
                        if (auto done = std::exchange(safe->onRemoved, nullptr))
                            done();
                });
        }
    }

    // The drawer drops out of the dock like a weighted tray, knocks against its end stop and bounces
    // twice before it rests; the first knock starts the power-on sequence.
    void tick()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        // Real elapsed time in small fixed steps, so the motion keeps its timing even when frames arrive slowly.
        auto elapsed = lastTick > 0 ? juce::jlimit(0.f, .25f, static_cast<float>((now - lastTick) * .001)) : 1.f / 60.f;
        lastTick = now;
        if (sliding)
        {
            const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((now - slideStart) * .001) / .4f);
            slideX = slideFromX * (1.f - easeOutBack(u));
            if (u >= 1.f)
            {
                sliding = false;
                slideX = 0;
            }
            placeSlide();
        }
        tickJolt(now);
        if (leaving)
        {
            tickLeave(now);
            return;
        }
        if (dropping)
            repaintTray(); // The drawer's shadow follows it as it drops.
        if (dockFalling)
        {
            // Lighter than a loaded drawer: it lands, hops and settles.
            constexpr float gravity = 2600.f, bounce = .32f, settle = 120.f, step = 1.f / 240.f;
            auto remaining = elapsed;
            while (remaining > 0 && dockFalling)
            {
                const auto dt = juce::jmin(step, remaining);
                remaining -= dt;
                dockSpeed += gravity * dt;
                dockDrop -= dockSpeed * dt;
                if (dockDrop > 0)
                    continue;
                dockDrop = 0;
                if (dockSpeed > settle)
                {
                    if (landedAt <= 0)
                    {
                        landedAt = now;
                        // An empty dock is lighter: a smaller knock.
                        if (onLanded)
                            onLanded(.45f * juce::jlimit(0.f, 1.f, dockSpeed / 700.f));
                    }
                    dockSpeed = -dockSpeed * bounce;
                }
                else
                {
                    dockSpeed = 0;
                    dockFalling = false;
                }
            }
            placeDock();
        }
        if (landedAt > 0)
        {
            insert.repaint();
            if (now - landedAt > 700)
                landedAt = 0;
        }
        if (dropping)
        {
            constexpr float gravity = 3400.f, bounce = .3f, settle = 160.f, step = 1.f / 240.f;
            while (elapsed > 0 && dropping)
            {
                const auto dt = juce::jmin(step, elapsed);
                elapsed -= dt;
                speed += gravity * dt;
                offset -= speed * dt;
                if (offset > 0)
                    continue;
                offset = 0;
                if (speed > settle)
                {
                    if (!booting)
                    {
                        booting = true;
                        sparkStart = now;
                        drawer->startBoot(now);
                        if (onLanded)
                            onLanded(juce::jlimit(0.f, 1.f, speed / 1400.f));
                    }
                    speed = -speed * bounce;
                }
                else
                {
                    speed = 0;
                    dropping = false;
                }
            }
            placeDrawer();
        }
        if (booting)
            booting = drawer->tickBoot(now);
        if (sparkStart > 0)
        {
            if (now - sparkStart > 430)
                sparkStart = 0;
            repaintTray();
        }
    }

    const CartridgeImages& art;

    juce::Component drawerWindow;
    std::array<PaintCache, 2> dockArt;
    float offset = 0, speed = 0;
    static constexpr float dockFall = 80.f;
    float dockDrop = 0, dockSpeed = 0;
    double lastTick = 0, landedAt = 0, sparkStart = 0;
    bool dropping = false, booting = false, dockFalling = false;
    bool leaving = false, sliding = false, awaiting = false;
    double leaveStart = 0, slideStart = 0;
    float slideX = 0, slideFromX = 0;
    bool jolting = false;
    double joltStart = 0;
    float joltStrength = 0, joltY = 0;
    std::function<void()> onRemoved;
    bool target = false;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// Layer above every card in the rack: a cartridge powering up crackles with current, and its light spills over
// the rack and the cards beside it. The charged panel itself is left out of the bloom so it doesn't wash out.
class RackSparks final : public juce::Component
{
public:
    RackSparks() { setInterceptsMouseClicks(false, false); }

    void paint(juce::Graphics& g) override
    {
        const auto* tray = getParentComponent();
        if (tray == nullptr)
            return;
        for (auto* child : tray->getChildren())
        {
            const auto* slot = dynamic_cast<CartridgeSlot*>(child);
            if (slot == nullptr || !slot->isVisible())
                continue;
            if (const auto charge = slot->sparkInRack())
            {
                {
                    const juce::Graphics::ScopedSaveState state(g);
                    g.excludeClipRegion(charge->panel.toNearestInt());
                    retro::electricBloom(g, charge->panel, charge->intensity * (.35f + .65f * charge->reach));
                }
                retro::electricFrame(g, charge->panel, charge->reach, charge->intensity, charge->frame);
            }
        }
    }
};

// Rack contents: paints the soft shadows the slots cast onto the rack floor, with the spark layer kept on top.
class RackTray final : public juce::Component
{
public:
    RackTray() { addAndMakeVisible(sparks); }

    void resized() override { sparks.setBounds(getLocalBounds()); }

    void childrenChanged() override
    {
        if (getIndexOfChildComponent(&sparks) != getNumChildComponents() - 1)
            sparks.toFront(false);
    }

    void paint(juce::Graphics& g) override
    {
        const juce::DropShadow shadow(juce::Colours::black.withAlpha(.85f), 18, { 4, 9 });
        const auto repainted = g.getClipBounds();
        for (auto* child : getChildren())
            if (auto* slot = dynamic_cast<CartridgeSlot*>(child); slot != nullptr && slot->isVisible())
                for (const auto& part : slot->silhouette())
                    if (!part.isEmpty() && part.expanded(32).intersects(repainted))
                        shadow.drawForRectangle(g, part);
    }

private:
    RackSparks sparks;
};

// The effects in the chain, in signal order, shown as plates along the Input -> Output line. Clicking one
// scrolls the rack to that cartridge.
class SignalChain final : public juce::Component
{
public:
    struct Entry
    {
        int instance;
        bool on;
        bool operator==(const Entry&) const = default;
    };

    SignalChain()
    {
        setComponentID("signal_chain");
        setTitle("Effect chain");
    }

    std::function<void(int position)> onSelect;

    void update(std::vector<Entry> chain)
    {
        if (chain != entries)
        {
            entries = std::move(chain);
            plates.clear();
            repaint();
        }
    }

    // The meters beside it repaint this bar many times a second, so the plates are kept as one picture.
    void paint(juce::Graphics& g) override
    {
        plates.draw(g, getLocalBounds(), [this](juce::Graphics& target)
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const auto& entry = entries[i];
                const auto type = entry.instance >= 0 ? static_cast<size_t>(entry.instance) % modules.size() : modules.size();
                const auto name = entry.instance < 0 ? juce::String("Empty")
                    : juce::String(modules[type].name) + (entry.instance >= static_cast<int>(modules.size()) ? " 2" : "");
                retro::chainChip(target, chip(static_cast<int>(i)), type, name, entry.on, static_cast<int>(i) == hovered);
            }
        });
    }

    void mouseMove(const juce::MouseEvent& event) override { setHovered(chipAt(event.position)); }
    void mouseExit(const juce::MouseEvent&) override { setHovered(-1); }
    void mouseUp(const juce::MouseEvent& event) override
    {
        if (const auto index = chipAt(event.position); index >= 0 && onSelect && event.mouseWasClicked())
            onSelect(index);
    }

    // A chip's place: the line is shared out evenly, each chip centred in its share.
    juce::Rectangle<float> chip(int index) const
    {
        const auto count = juce::jmax(1, static_cast<int>(entries.size()));
        const auto share = static_cast<float>(getWidth()) / static_cast<float>(count);
        const auto width = juce::jmin(118.f, share - 8.f);
        return juce::Rectangle<float>(width, static_cast<float>(getHeight()))
            .withCentre({ share * (static_cast<float>(index) + .5f), static_cast<float>(getHeight()) * .5f });
    }

private:
    int chipAt(juce::Point<float> point) const
    {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            if (chip(i).expanded(2.f).contains(point))
                return i;
        return -1;
    }

    void setHovered(int index)
    {
        if (std::exchange(hovered, index) != index)
        {
            setMouseCursor(index >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            plates.clear();
            repaint();
        }
    }

    std::vector<Entry> entries;
    int hovered = -1;
    PaintCache plates;
};

// Soft shadow along the rack window's edges where cards continue out of view, so they slide under the
// housing instead of being cut off. It lies above the rack but below a carried cartridge.
class RackEdgeShade final : public juce::Component
{
public:
    explicit RackEdgeShade(const juce::Viewport& rackView) : view(rackView)
    {
        setInterceptsMouseClicks(false, false);
    }

    void paint(juce::Graphics& g) override
    {
        const auto* content = view.getViewedComponent();
        if (content == nullptr)
            return;
        const auto area = getLocalBounds().toFloat();
        const auto shade = [&g, area](float edge, float inner)
        {
            juce::ColourGradient fade(juce::Colours::black.withAlpha(.82f), edge, 0, juce::Colours::transparentBlack, inner, 0, false);
            fade.addColour(.35, juce::Colours::black.withAlpha(.45f));
            g.setGradientFill(fade);
            g.fillRect(juce::Rectangle<float>(juce::jmin(edge, inner), area.getY(), std::abs(inner - edge), area.getHeight()));
        };
        constexpr float depth = 36.f;
        const auto x = view.getViewPositionX();
        if (x + view.getViewWidth() < content->getWidth())
            shade(area.getRight(), area.getRight() - depth);
        if (x > 0)
            shade(area.getX(), area.getX() + depth);
    }

private:
    const juce::Viewport& view;
};

// The cartridge carried by the mouse. It hangs from the point where it was grabbed, trails the hand on a
// spring, swings like a pendulum when the hand accelerates sideways, is pulled towards a free slot under it
// and finally settles into its slot or drifts back to where it came from.
class DragGhost final : public juce::Component
{
public:
    using Point = juce::Point<float>;
    using Box = juce::Rectangle<float>;

    explicit DragGhost(const CartridgeImages& images) : art(images)
    {
        setComponentID("drag_ghost");
        setInterceptsMouseClicks(false, false);
        setSize(juce::roundToInt(baseWidth + margin * 2), juce::roundToInt(baseHeight + margin * 2));
    }

    // `home` is where the cartridge lies before it is lifted, `grab` the point the mouse holds it by.
    void pickUp(int cartridgeType, Box home, Point grab)
    {
        type = cartridgeType;
        homeBox = home;
        scale = home.getWidth() / baseWidth;
        pivot = { margin + juce::jlimit(0.f, baseWidth, (grab.x - home.getX()) / scale),
                  margin + juce::jlimit(0.f, baseHeight, (grab.y - home.getY()) / scale) };
        position = cursor = grab;
        velocity = {};
        angle = spin = lift = 0;
        magnet = {};
        mode = Mode::carry;
        lastTick = juce::Time::getMillisecondCounterHiRes();
        setVisible(true);
        toFront(false);
        place();
    }

    bool carrying() const { return mode == Mode::carry; }

    // `snap` is the slot the cartridge would drop into from here, or empty.
    void follow(Point mouse, Box snap)
    {
        cursor = mouse;
        magnet = snap;
        if (!animated())
        {
            position = mouse;
            place();
        }
    }

    // Settles into `slot`, the area a seated cartridge fills; an empty area sends it back home.
    void drop(Box slot)
    {
        if (mode == Mode::idle)
            return;
        mode = slot.isEmpty() ? Mode::home : Mode::land;
        landing = slot.isEmpty() ? homeBox : slot;
        if (!animated())
            finish();
    }

    void paint(juce::Graphics& g) override
    {
        const auto card = Box(margin, margin, baseWidth, baseHeight);
        // The shadow lengthens and softens the higher the cartridge is lifted off the deck.
        if (lift > .01f)
            juce::DropShadow(juce::Colours::black.withAlpha(.6f * lift), juce::roundToInt(6 + 16 * lift),
                             { juce::roundToInt(7 * lift), juce::roundToInt(16 * lift) })
                .drawForRectangle(g, card.toNearestInt());
        art.drawCartridge(g, card.toNearestInt(), type, false, 1.f);
        if (lift > .05f)
            art.drawDock(g, Area(68, 12).withCentre({ juce::roundToInt(card.getCentreX()),
                                                       juce::roundToInt(card.getBottom()) + 2 }), true, lift);
    }

private:
    enum class Mode { idle, carry, land, home };
    static constexpr float baseWidth = 140.f, baseHeight = baseWidth / cartridgeAspect, margin = 36.f;

    // Frames only arrive while the editor is on screen; elsewhere, e.g. in tests, the ghost jumps straight to its end.
    bool animated() const { return getPeer() != nullptr; }

    // Where the grabbed point sits when the cartridge lies flat, upright, filling `card`.
    Point anchorFor(Box card) const
    {
        return card.getTopLeft() + (pivot - Point(margin, margin)) * (card.getWidth() / baseWidth);
    }

    void tick()
    {
        if (mode == Mode::idle)
            return;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        auto elapsed = juce::jlimit(0.f, .25f, static_cast<float>((now - lastTick) * .001));
        lastTick = now;
        // Real elapsed time in small fixed steps keeps the motion's timing even when frames arrive slowly.
        constexpr float step = 1.f / 240.f;
        while (elapsed > 0 && mode != Mode::idle)
        {
            const auto dt = juce::jmin(step, elapsed);
            elapsed -= dt;
            simulate(dt);
        }
        if (mode != Mode::idle)
            place();
    }

    void simulate(float dt)
    {
        const bool held = mode == Mode::carry;
        auto target = cursor;
        auto targetScale = 1.08f; // Lifted a little towards the viewer.
        if (held && !magnet.isEmpty())
        {
            constexpr float pull = .35f;
            target = cursor + (anchorFor(magnet) - cursor) * pull;
            targetScale += (magnet.getWidth() / baseWidth - targetScale) * pull;
        }
        if (!held)
        {
            target = anchorFor(landing);
            targetScale = landing.getWidth() / baseWidth;
        }

        // Spring towards the hand: soft with a little overshoot while carried, firm when settling.
        const auto stiffness = held ? 520.f : 900.f;
        const auto damping = 2.f * std::sqrt(stiffness) * (held ? .62f : .95f);
        const auto acceleration = (target - position) * stiffness - velocity * damping;
        velocity += acceleration * dt;
        position += velocity * dt;

        // Pendulum: sideways acceleration of the hand swings the cartridge, a spring rights it again.
        spin += (acceleration.x * .0021f - angle * 170.f - spin * (held ? 8.f : 24.f)) * dt;
        angle = juce::jlimit(-.55f, .55f, angle + spin * dt);

        const auto ease = 1.f - std::exp(-dt * (held ? 12.f : 16.f));
        scale += (targetScale - scale) * ease;
        lift += ((held ? 1.f : 0.f) - lift) * ease;

        if (!held && target.getDistanceFrom(position) < .5f && velocity.getDistanceFromOrigin() < 15.f
            && std::abs(angle) < .004f && std::abs(targetScale - scale) < .004f)
            finish();
    }

    void place()
    {
        setTransform(juce::AffineTransform::translation(-pivot.x, -pivot.y)
                         .scaled(scale)
                         .rotated(angle)
                         .translated(position.x, position.y));
        repaint();
    }

    void finish()
    {
        mode = Mode::idle;
        setVisible(false);
        setTransform({});
    }

    const CartridgeImages& art;
    Mode mode = Mode::idle;
    int type = 0;
    Box homeBox, landing, magnet;
    Point pivot, position, velocity, cursor;
    float angle = 0, spin = 0, scale = 1, lift = 0;
    double lastTick = 0;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// A cartridge Generate deals into the rack: it lifts off the shelf, gathers with the others in the middle of the
// deck like a fanned hand, then is tossed over its slot and drops into the dock. Boxes are in the parent's coordinates.
class FlyingCartridge final : public juce::Component
{
public:
    using Point = juce::Point<float>;
    using Box = juce::Rectangle<float>;

    struct Flight
    {
        Box from, gather, to;
        float fanAngle = 0, bob = 0;
        double launch = 0, deal = 0; // When it leaves the shelf and when it leaves the hand, in ms.
        bool fade = false;           // Its slot lies outside the visible rack, so it fades on the way down.
    };

    FlyingCartridge(const CartridgeImages& images, int cartridgeType, Flight path, std::function<void()> landed)
        : art(images), type(cartridgeType), flight(path), onLanded(std::move(landed))
    {
        setInterceptsMouseClicks(false, false);
        setSize(juce::roundToInt(baseWidth + margin * 2), juce::roundToInt(baseHeight + margin * 2));
        place(flight.from, 0);
    }

    void paint(juce::Graphics& g) override
    {
        const auto card = Box(margin, margin, baseWidth, baseHeight);
        if (lift > .01f)
            juce::DropShadow(juce::Colours::black.withAlpha(.55f * lift), juce::roundToInt(6 + 18 * lift),
                             { juce::roundToInt(6 * lift), juce::roundToInt(18 * lift) })
                .drawForRectangle(g, card.toNearestInt());
        art.drawCartridge(g, card.toNearestInt(), type, false, 1.f);
    }

    static constexpr double riseTime = 520, dropTime = 600;

private:
    static constexpr float baseWidth = 140.f, baseHeight = baseWidth / cartridgeAspect, margin = 44.f;

    static float smooth(float u) { return u * u * (3.f - 2.f * u); }
    static float inOut(float u) { return u < .5f ? 4.f * u * u * u : 1.f - std::pow(-2.f * u + 2.f, 3.f) / 2.f; }
    static Box lerp(Box a, Box b, float u)
    {
        const auto width = a.getWidth() + (b.getWidth() - a.getWidth()) * u;
        return Box(width, width / cartridgeAspect);
    }

    void tick()
    {
        if (finished)
            return;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now < flight.launch)
            return;
        if (!isVisible())
        {
            setVisible(true);
            toFront(false);
        }
        const auto from = flight.from.getCentre(), gather = flight.gather.getCentre(), to = flight.to.getCentre();
        if (now < flight.deal)
        {
            // Off the shelf: it lifts straight up first, then swings over to its place in the hand.
            const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((now - flight.launch) / riseTime));
            const auto e = inOut(u);
            const Point control(from.x, gather.y - 10.f);
            auto at = from * ((1 - e) * (1 - e)) + control * (2 * e * (1 - e)) + gather * (e * e);
            // Held in the hand, it breathes a little until it is dealt.
            if (u >= 1.f)
                at.y += 2.2f * std::sin(static_cast<float>(now * .0062) + flight.bob);
            lift = smooth(juce::jmin(1.f, u * 1.6f));
            place(lerp(flight.from, flight.gather, e).withCentre(at), flight.fanAngle * e + .1f * std::sin(u * juce::MathConstants<float>::pi));
            return;
        }

        // Dealt: tossed up and over its slot, then it falls faster and faster into the dock.
        const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((now - flight.deal) / dropTime));
        const auto across = inOut(juce::jmin(1.f, u * 1.15f));
        const auto x = gather.x + (to.x - gather.x) * across;
        const auto y = gather.y + (to.y - gather.y) * u * u - 64.f * std::sin(u * juce::MathConstants<float>::pi) * (1.f - u);
        const auto turn = flight.fanAngle >= 0 ? 1.f : -1.f;
        const auto angle = flight.fanAngle * (1.f - across) + turn * .16f * std::sin(u * juce::MathConstants<float>::pi);
        lift = 1.f - smooth(juce::jlimit(0.f, 1.f, (u - .55f) / .45f));
        if (flight.fade)
            setAlpha(1.f - smooth(juce::jlimit(0.f, 1.f, (u - .45f) / .45f)));
        place(lerp(flight.gather, flight.to, smooth(u)).withCentre({ x, y }), angle);
        if (u >= 1.f)
        {
            finished = true;
            setVisible(false);
            if (onLanded)
                onLanded();
        }
    }

    void place(Box box, float angle)
    {
        // Lifted cards come a touch towards the viewer.
        const auto scale = box.getWidth() / baseWidth * (1.f + .1f * lift);
        const Point pivot(margin + baseWidth / 2, margin + baseHeight / 2);
        setTransform(juce::AffineTransform::translation(-pivot.x, -pivot.y)
                         .scaled(scale)
                         .rotated(angle)
                         .translated(box.getCentreX(), box.getCentreY()));
        repaint();
    }

    const CartridgeImages& art;
    const int type;
    const Flight flight;
    std::function<void()> onLanded;
    float lift = 0;
    bool finished = false;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

class CartridgeInspector final : public juce::Component
{
public:
    CartridgeInspector(juce::AudioProcessor& processor, juce::AudioProcessorValueTreeState& state,
                       juce::String title, juce::String prefix, bool global, const CartridgeImages& artwork,
                       int delay, int stretch = -1)
        : art(artwork), routes(state), type(global ? modules.size() : moduleForPrefix(prefix)), delayInstance(delay),
          stretchInstance(stretch),
          caption(std::move(title)), active(type < modules.size() ? state.getRawParameterValue(prefix + "active")
                                                                  : nullptr)
    {
        setComponentID("effects_inspector");
        setTitle(caption);
        styleButton(close, ButtonFace::inspectorClose);
        close.setTitle("Close controls");
        close.setComponentID("close_controls");
        addAndMakeVisible(close);
        addAndMakeVisible(view);
        if (delayInstance >= 0)
        {
            envelope.setComponentID("echo_envelope");
            addAndMakeVisible(envelope);
        }
        if (stretchInstance >= 0)
        {
            map.setComponentID("stretch_map");
            map.attach(state.getParameter(prefix + "start"), state.getParameter(prefix + "land"));
            addAndMakeVisible(map);
        }
        view.setViewedComponent(&grid, false);
        view.setScrollBarsShown(true, false);
        view.setScrollBarThickness(8);

        // Captions are printed in the ink that reads on this effect's finish.
        const auto captionColour = type < modules.size() ? CartridgeImages::panelText(type) : cream.withAlpha(.94f);
        for (auto* item : processor.getParameters())
        {
            auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(item);
            if (parameter == nullptr)
                continue;
            const auto& id = parameter->paramID;
            const auto matches = global ? id.startsWith("global_") || id.startsWith("eq_")
                                                || id == "input_gain" || id == "output_gain"
                                      : id.startsWith(prefix);
            if (!matches || id.endsWith("_slot") || (!global && id == prefix + "active"
                    && prefix != "lfo_1_" && prefix != "lfo_2_"))
                continue;
            auto tile = std::make_unique<ParameterTile>(state, *parameter, controlName(*parameter, prefix),
                                                        type, captionColour, false);
            grid.addAndMakeVisible(*tile);
            tiles.push_back(std::move(tile));
        }
    }

    void paint(juce::Graphics& g) override
    {
        {
            // The backdrop fades out over its last stretch, so cables and the shelf pass softly into the master
            // section below.
            const auto bounds = getLocalBounds().toFloat();
            const auto dim = juce::Colour(0xff071015).withAlpha(.68f * dimming);
            constexpr float fade = 46.f;
            g.setColour(dim);
            g.fillRect(bounds.withTrimmedBottom(fade));
            g.setGradientFill(juce::ColourGradient(dim, 0, bounds.getBottom() - fade, dim.withAlpha(0.f), 0, bounds.getBottom(), false));
            g.fillRect(bounds.withTop(bounds.getBottom() - fade));
        }
        if (presence <= .001f)
            return;
        const juce::Graphics::ScopedSaveState state(g);
        g.addTransform(motion);
        const bool fading = presence < .999f;
        if (fading)
            g.beginTransparencyLayer(presence);
        // While it opens or closes the plate is redrawn each frame at its changing size; at rest it is cached.
        if (phase == Phase::idle)
            plateArt.draw(g, plate.expanded(56), [this](juce::Graphics& cg) { paintPlate(cg); });
        else
            paintPlate(g);
        if (active != nullptr)
            art.drawSprite(g, CartridgeImages::lampOn, lampArea, engaged ? 1.f : .24f);
        retro::title(g, captionArea, caption, type, 22.f);
        if (fading)
            g.endTransparencyLayer();
    }

    // Cables from the card's input come out of the socket in the header and run to the knobs they drive.
    void paintOverChildren(juce::Graphics& g) override
    {
        // The leads come and go with the window, fading and sliding with it.
        if (presence <= .01f || type >= modules.size())
            return;
        const juce::Graphics::ScopedSaveState moving(g);
        g.addTransform(motion);
        const auto fading = presence < .999f;
        if (fading)
            g.beginTransparencyLayer(presence);
        // The socket itself is part of the plate (see paintPlate).
        const auto socket = inputPoint();
        const auto window = view.getBounds().toFloat();
        std::map<juce::Slider*, int> seen;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        for (const auto& [slider, colour] : routedKnobs())
        {
            // A lead patched while the window is open waits for the rack's cable to settle, then runs out of IN.
            const auto birth = leadBirth.find({ slider, colour.getARGB() });
            // One the window has not taken in yet (patched since the last refresh) waits for it.
            const auto reach = birth == leadBirth.end() ? (knownLeads && phase == Phase::idle ? 0.f : 1.f)
                             : birth->second <= 0.0 ? 1.f
                             : juce::jlimit(0.f, 1.f, static_cast<float>((now - birth->second) * .001 / leadRun));
            if (reach <= 0.f)
                continue;
            const auto* tile = slider->getParentComponent();
            auto knob = dynamic_cast<const ParameterTile*>(tile) != nullptr
                ? const_cast<ParameterTile*>(dynamic_cast<const ParameterTile*>(tile))->knob() : juce::Rectangle<float>();
            if (knob.isEmpty())
                continue;
            const auto end = getLocalPoint(tile, knob.getCentre() + juce::Point<float>(.94f, -.34f) * (knob.getWidth() * .5f + 8.f))
                           + juce::Point<float>(0, 3.f * static_cast<float>(seen[slider]++));
            const auto shown = window.contains(end);
            const auto to = shown ? end : juce::Point<float>(juce::jlimit(window.getX(), window.getRight(), end.x),
                                                             end.y < window.getY() ? window.getY() : window.getBottom());
            const auto sag = juce::jmax(20.f, std::abs(to.x - socket.x) * .18f);
            const auto c1 = socket.translated(0, sag), c2 = to.translated(0, -sag);
            const auto along = [&](float u)
            {
                const auto v = 1.f - u;
                return socket * (v * v * v) + c1 * (3.f * v * v * u) + c2 * (3.f * v * u * u) + to * (u * u * u);
            };
            const auto span = 1.f - std::pow(1.f - reach, 3.f);
            juce::Path lead;
            lead.startNewSubPath(socket);
            constexpr int steps = 28;
            for (int i = 1; i <= steps; ++i)
                lead.lineTo(along(span * static_cast<float>(i) / steps));
            const juce::Graphics::ScopedSaveState state(g);
            g.setColour(juce::Colours::black.withAlpha(.35f));
            g.strokePath(lead, juce::PathStrokeType(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                         juce::AffineTransform::translation(1.f, 2.f));
            g.setColour(colour.withAlpha(shown ? .95f : .5f));
            g.strokePath(lead, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            if (reach < 1.f || shown)
            {
                const auto tip = along(span);
                g.setColour(colour.darker(.3f));
                g.fillEllipse(juce::Rectangle<float>(7.f, 7.f).withCentre(tip));
                g.setColour(juce::Colours::white.withAlpha(.5f));
                g.fillEllipse(juce::Rectangle<float>(2.f, 2.f).withCentre(tip.translated(-1.f, -1.f)));
            }
        }
        if (fading)
            g.endTransparencyLayer();
    }

    // Where the header's input socket sits, left of the nameplate.
    juce::Point<float> inputPoint() const
    {
        return { static_cast<float>(plate.getRight() - 342), static_cast<float>(plate.getY() + 32) };
    }

    // The plate, while it rests open; empty while it moves.
    juce::Rectangle<float> restingPlate() const
    {
        return phase == Phase::idle && presence >= .999f ? plate.toFloat() : juce::Rectangle<float>();
    }

    // The knob in this window for a parameter id, if it has one.
    juce::Slider* knobFor(const juce::String& id) const
    {
        for (const auto& tile : tiles)
            for (const auto& name : { id, "cart2_" + id })
                if (auto* slider = dynamic_cast<juce::Slider*>(tile->findChildWithID(name)))
                    return slider;
        return nullptr;
    }

    void paintPlate(juce::Graphics& g) const
    {
        juce::DropShadow(juce::Colours::black.withAlpha(.9f), 34, { 0, 14 }).drawForRectangle(g, plate.reduced(8));
        art.drawInspector(g, plate, type);
        // The card's input comes out here: an IN socket in the header, mounted with the plate.
        if (type < modules.size())
        {
            const auto socket = inputPoint();
            paintPanelSocket(g, socket, 14.f, 1.f, false);
            g.setFont(font(9.f, true));
            g.setColour(CartridgeImages::panelText(type).withAlpha(.7f));
            g.drawText("IN", juce::Rectangle<float>(20.f, 10.f).withCentre(socket.translated(0, -13.f)), juce::Justification::centred);
        }
        if (grid.getHeight() > view.getHeight())
        {
            // A long panel scrolls inside a sunken window cut into the plate, so its controls stay on the card.
            const auto well = view.getBounds().toFloat().expanded(7, 6);
            g.setColour(juce::Colours::black.withAlpha(.22f));
            g.fillRoundedRectangle(well, 7);
            g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(.5f), 0, well.getY(),
                                                   juce::Colours::transparentBlack, 0, well.getY() + 12, false));
            g.fillRoundedRectangle(well, 7);
            g.setGradientFill(juce::ColourGradient(juce::Colours::transparentBlack, 0, well.getBottom() - 12,
                                                   juce::Colours::black.withAlpha(.45f), 0, well.getBottom(), false));
            g.fillRoundedRectangle(well, 7);
            g.setColour(juce::Colours::black.withAlpha(.6f));
            g.drawRoundedRectangle(well, 7, 1);
            g.setColour(juce::Colours::white.withAlpha(.14f));
            g.drawLine(well.getX() + 7, well.getBottom() + 1, well.getRight() - 7, well.getBottom() + 1, 1);
        }
    }

    // Opens out of `origin`, the card that was clicked (in this component's coordinates): the panel grows
    // from it to full size with a springy overshoot while the room dims, then its controls rise in one by one
    // and every knob runs its power-on sweep.
    // Fades in while sliding up a few pixels; its controls are simply there.
    void open(Area)
    {
        phase = Phase::opening;
        started = juce::Time::getMillisecondCounterHiRes();
        if (getPeer() == nullptr)
        {
            settle();
            return;
        }
        apply(0.f);
    }

    // Folds the panel back into its card, then calls `done`.
    void dismiss(std::function<void()> done)
    {
        onDismissed = std::move(done);
        if (getPeer() == nullptr || phase == Phase::closing)
        {
            finishDismiss();
            return;
        }
        phase = Phase::closing;
        started = juce::Time::getMillisecondCounterHiRes();
    }

    void resized() override
    {
        const auto count = static_cast<int>(tiles.size());
        // Up to six columns, so most panels fit in two rows without scrolling; a long panel (Grain) gets up to eight,
        // on a wider plate.
        const auto columns = count > 18 ? juce::jlimit(6, 8, (count + 2) / 3) : juce::jlimit(3, 6, (count + 1) / 2);
        const auto rows = (count + columns - 1) / columns;
        constexpr int rowHeight = 108, rowSpacing = 15;
        const auto gridHeight = rows * rowHeight + juce::jmax(0, rows - 1) * rowSpacing;
        plateArt.clear();
        const auto room = getLocalBounds().reduced(17, 11);
        // The plate keeps its proportions for short panels and grows taller, up to the room it has, for long ones.
        const auto plateWidth = juce::jmin(room.getWidth(), 640 + 105 * juce::jmax(0, columns - 6));
        const auto extras = (delayInstance >= 0 ? 60 : 0) + (stretchInstance >= 0 ? 84 : 0);
        const auto needed = 16 + 64 + 4 + extras + 8 + gridHeight + 5 + 21;
        plate = art.fitInspector(room.withSizeKeepingCentre(plateWidth, juce::jmin(369, room.getHeight())), type);
        if (needed > plate.getHeight())
            plate = room.withSizeKeepingCentre(plateWidth, juce::jmin(needed, room.getHeight()));

        auto area = plate.reduced(25, 0).withTrimmedTop(16).withTrimmedBottom(21);
        auto heading = area.removeFromTop(38).reduced(19, 0);
        // Centred in the plate's 64 px header band (see retro::inspectorPlate).
        const auto headingY = plate.getY() + 32;
        heading.translate(0, headingY - heading.getCentreY());
        close.setBounds(heading.removeFromRight(30).withSizeKeepingCentre(30, 30));
        if (active != nullptr)
            lampArea = heading.removeFromLeft(22).withSizeKeepingCentre(12, 12);
        captionArea = heading.withTrimmedLeft(4);

        // Everything below starts clear of the plate's 64 px header band, leaving room for the rim of the sunken
        // scroll window (drawn 6 px outside the viewport) so it never cuts into the header's finish.
        constexpr int headerBand = 64, wellRim = 8;
        area.setTop(plate.getY() + headerBand + 4);
        if (delayInstance >= 0)
            envelope.setBounds(area.removeFromTop(96).reduced(4, 3));
        if (stretchInstance >= 0)
            map.setBounds(area.removeFromTop(84).reduced(4, 3));
        area.removeFromTop(wellRim);
        // A scrolling window also keeps clear of the trim along the plate's bottom edge.
        if (gridHeight >= area.getHeight())
            area.removeFromBottom(5);
        if (gridHeight < area.getHeight())
            area = area.withSizeKeepingCentre(area.getWidth(), gridHeight);
        view.setBounds(area);
        const int gutter = gridHeight > view.getHeight() ? view.getScrollBarThickness() : 0;
        grid.setSize(view.getWidth() - gutter, gridHeight);
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            const auto index = static_cast<int>(i);
            const auto column = index % columns;
            const auto left = grid.getWidth() * column / columns;
            const auto right = grid.getWidth() * (column + 1) / columns;
            tiles[i]->setBounds(left + 3, (index / columns) * (rowHeight + rowSpacing),
                                right - left - 6, rowHeight);
        }
    }

    juce::TextButton close { "Close" };

    void refresh(EditorModel& model)
    {
        // Leads present when the window opens are simply there; one that appears while it is open is timed in.
        {
            std::map<std::pair<juce::Slider*, juce::uint32>, double> next;
            const auto animate = phase == Phase::idle && getPeer() != nullptr && knownLeads;
            for (const auto& [slider, colour] : routedKnobs())
            {
                const std::pair key { slider, colour.getARGB() };
                const auto found = leadBirth.find(key);
                next[key] = found != leadBirth.end() ? found->second
                          : animate ? juce::Time::getMillisecondCounterHiRes() + leadDelay * 1000.0 : 0.0;
            }
            leadBirth = std::move(next);
            knownLeads = knownLeads || phase == Phase::idle;
        }
        // Redraw the breakout when a route changes or the panel scrolls.
        auto signature = static_cast<double>(view.getViewPositionY());
        for (const auto& [slider, colour] : routedKnobs())
            signature = signature * 31.0 + static_cast<double>(reinterpret_cast<std::uintptr_t>(slider) % 9973) + colour.getARGB() % 101;
        if (std::exchange(routeSignature, signature) != signature)
            repaint();
        if (delayInstance >= 0)
            envelope.update(model.delayPreview(delayInstance));
        if (stretchInstance >= 0)
            map.update(model.stretchPreview(stretchInstance));
        if (active != nullptr)
        {
            const auto newState = active->load(std::memory_order_relaxed) >= .5f;
            if (std::exchange(engaged, newState) != newState)
                repaint(lampArea);
        }
    }

private:
    enum class Phase { idle, opening, closing };

    // The knobs in this window that a live route drives, with the colour of its source.
    std::vector<std::pair<juce::Slider*, juce::Colour>> routedKnobs() const
    {
        std::vector<std::pair<juce::Slider*, juce::Colour>> result;
        for (size_t s = 0; s < sourcePrefixes.size(); ++s)
        {
            const juce::String prefix(sourcePrefixes[s]);
            if (const auto* on = routes.getRawParameterValue(prefix + "active"); on != nullptr && on->load() < .5f)
                continue;
            for (int slot = 1; slot <= 4; ++slot)
            {
                const auto suffix = juce::String(slot);
                const auto* target = routes.getRawParameterValue(prefix + "target_" + suffix);
                const auto* depth = routes.getRawParameterValue(prefix + "depth_" + suffix);
                const auto* active = routes.getRawParameterValue(prefix + "active_" + suffix);
                if (target == nullptr || depth == nullptr || depth->load() == 0 || (active != nullptr && active->load() < .5f))
                    continue;
                const auto t = juce::roundToInt(target->load());
                if (t <= 0 || t >= static_cast<int>(modulationKnobs.size()))
                    continue;
                if (auto* slider = knobFor(modulationKnobs[static_cast<size_t>(t)]))
                    result.emplace_back(slider, juce::Colour(sourceColours[s]));
            }
        }
        return result;
    }

    static size_t moduleForPrefix(const juce::String& prefix)
    {
        const auto local = prefix.startsWith("cart2_") ? prefix.substring(6) : prefix;
        for (size_t i = 0; i < modules.size(); ++i)
            if (local == juce::String(modules[i].prefix) + "_")
                return i;
        return modules.size();
    }

    void tick()
    {
        if (phase == Phase::idle)
        {
            // Keep drawing while a new lead runs out.
            const auto now = juce::Time::getMillisecondCounterHiRes();
            if (std::any_of(leadBirth.begin(), leadBirth.end(), [now](const auto& entry)
                    { return entry.second > 0.0 && now < entry.second + leadRun * 1000.0 + 50.0; }))
                repaint();
            return;
        }
        const auto t = static_cast<float>((juce::Time::getMillisecondCounterHiRes() - started) * .001);
        if (phase == Phase::closing)
        {
            const auto u = juce::jlimit(0.f, 1.f, t / .16f);
            apply(1.f - u, 8.f * u);
            if (u >= 1.f)
                finishDismiss();
            return;
        }
        const auto u = juce::jlimit(0.f, 1.f, t / .2f);
        const auto eased = 1.f - (1.f - u) * (1.f - u) * (1.f - u);
        apply(eased, 12.f * (1.f - eased));
        if (u >= 1.f)
            settle();
    }

    // `opacity` fades the window and the room's dimming behind it; `lift` is how far below its place it sits.
    void apply(float opacity, float lift = 0.f)
    {
        presence = opacity;
        motion = juce::AffineTransform::translation(0.f, lift);
        dimming = opacity;
        for (auto* part : { static_cast<juce::Component*>(&close), static_cast<juce::Component*>(&view),
                            static_cast<juce::Component*>(&envelope), static_cast<juce::Component*>(&map) })
        {
            part->setTransform(motion);
            part->setAlpha(opacity);
        }
        repaint();
    }

    void settle()
    {
        phase = Phase::idle;
        motion = {};
        dimming = 1.f;
        presence = 1.f;
        for (auto* part : { static_cast<juce::Component*>(&close), static_cast<juce::Component*>(&view),
                            static_cast<juce::Component*>(&envelope), static_cast<juce::Component*>(&map) })
        {
            part->setTransform({});
            part->setAlpha(1.f);
        }
        for (auto& tile : tiles)
        {
            tile->setAlpha(1.f);
            tile->setTransform({});
            tile->setDrop(0.f);
            tile->setSweep(-1.f);
        }
        repaint();
    }

    void finishDismiss()
    {
        phase = Phase::idle;
        if (auto done = std::exchange(onDismissed, nullptr))
            juce::MessageManager::callAsync(std::move(done));
    }

    const CartridgeImages& art;
    juce::AudioProcessorValueTreeState& routes;
    double routeSignature = 0;
    // When each lead in the window started to run out (0 for leads that are simply there).
    std::map<std::pair<juce::Slider*, juce::uint32>, double> leadBirth;
    bool knownLeads = false;
    static constexpr double leadDelay = .45, leadRun = .3;
    const size_t type;
    const int delayInstance, stretchInstance;
    const juce::String caption;
    const std::atomic<float>* active;
    bool engaged = false;
    Area plate, captionArea, lampArea, source;
    EchoEnvelope envelope;
    StretchMap map { false };
    PaintCache plateArt;
    juce::Component grid;
    juce::Viewport view;
    std::vector<std::unique_ptr<ParameterTile>> tiles;
    Phase phase = Phase::idle;
    double started = 0;
    juce::AffineTransform motion;
    float dimming = 1.f, presence = 1.f;
    std::function<void()> onDismissed;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// A notice plate that drops down over the top of the rack instead of a system dialog: an amber lamp pulses
// beside the message, and it slides away again after a few seconds or when clicked.
class NoticePlate final : public juce::Component, private juce::Timer
{
public:
    NoticePlate()
    {
        setComponentID("notice");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void show(juce::String heading, juce::String message)
    {
        title = std::move(heading);
        text = std::move(message);
        setTitle(title);
        setDescription(text);
        shownAt = juce::Time::getMillisecondCounterHiRes();
        leaving = false;
        setVisible(true);
        toFront(false);
        if (auto* handler = getAccessibilityHandler())
            handler->postAnnouncement(title + ". " + text, juce::AccessibilityHandler::AnnouncementPriority::high);
        startTimer(3600);
        place();
    }

    void paint(juce::Graphics& g) override
    {
        const auto t = static_cast<float>((juce::Time::getMillisecondCounterHiRes() - shownAt) * .001);
        const auto glow = .55f + .45f * std::cos(t * juce::MathConstants<float>::twoPi * 1.6f);
        retro::notice(g, getLocalBounds(), title, text, glow);
    }

    void mouseUp(const juce::MouseEvent&) override { dismiss(); }

private:
    void timerCallback() override { dismiss(); }

    void dismiss()
    {
        stopTimer();
        if (getPeer() == nullptr)
        {
            setVisible(false);
            return;
        }
        if (!leaving)
        {
            leaving = true;
            leftAt = juce::Time::getMillisecondCounterHiRes();
        }
    }

    // Drops in on a spring from above, then rises out of view when leaving.
    void place()
    {
        if (!isVisible())
            return;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        float drop = 0, opacity = 1;
        if (leaving)
        {
            const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((now - leftAt) * .001) / .22f);
            drop = -u * u * static_cast<float>(getHeight() + 20);
            opacity = 1.f - u;
            if (u >= 1.f)
            {
                setVisible(false);
                setTransform({});
                return;
            }
        }
        else if (getPeer() != nullptr)
        {
            const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((now - shownAt) * .001) / .38f);
            drop = -(1.f - easeOutBack(u)) * static_cast<float>(getHeight() + 20);
            opacity = juce::jmin(1.f, u * 3.f);
        }
        setTransform(juce::AffineTransform::translation(0, drop));
        setAlpha(opacity);
        repaint();
    }

    juce::String title, text;
    double shownAt = 0, leftAt = 0;
    bool leaving = false;
    juce::VBlankAttachment frames { this, [this] { place(); } };
};

constexpr int editorDesignWidth = 1160, editorDesignHeight = 688;

// The Tape Stop key: a chunky transport key with a red lamp. Pressing it latches the stop; pressing again starts
// the tape. Held MIDI notes and automation stop the tape too.
class TransportKey final : public juce::Button
{
public:
    TransportKey() : juce::Button("Tape Stop")
    {
        setComponentID("tape_stop");
        setClickingTogglesState(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setMouseClickGrabsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        const auto stopped = getToggleState();
        auto area = getLocalBounds().toFloat().reduced(1.f);
        // The key sits a little lower while latched, like a pressed transport key.
        const auto sink = stopped || down ? 1.5f : 0.f;
        g.setColour(juce::Colours::black.withAlpha(.55f));
        g.fillRoundedRectangle(area.translated(0, 2.f), 5);
        area = area.withTrimmedBottom(2.f).translated(0, sink);
        juce::ColourGradient face(juce::Colour(over ? 0xff4a4e53 : 0xff3d4146), 0, area.getY(),
                                  juce::Colour(0xff1b1d20), 0, area.getBottom(), false);
        g.setGradientFill(face);
        g.fillRoundedRectangle(area, 5);
        g.setColour(juce::Colours::white.withAlpha(stopped ? .06f : .16f));
        g.drawHorizontalLine(juce::roundToInt(area.getY() + 1), area.getX() + 4, area.getRight() - 4);
        g.setColour(juce::Colours::black.withAlpha(.8f));
        g.drawRoundedRectangle(area, 5, 1);

        // Lamp, square stop symbol and caption.
        auto inner = area.reduced(8, 5);
        const auto lamp = inner.removeFromLeft(9).withSizeKeepingCentre(9, 9);
        const juce::Colour red(0xffff4436);
        if (stopped)
        {
            g.setColour(red.withAlpha(.3f));
            g.fillEllipse(lamp.expanded(3));
        }
        g.setColour(stopped ? red : juce::Colour(0xff40201d));
        g.fillEllipse(lamp);
        g.setColour(juce::Colours::white.withAlpha(stopped ? .6f : .15f));
        g.fillEllipse(lamp.withSizeKeepingCentre(3, 3).translated(-1, -1.5f));
        inner.removeFromLeft(6);
        const auto symbol = inner.removeFromLeft(9).withSizeKeepingCentre(8, 8);
        g.setColour(cream.withAlpha(.9f));
        g.fillRect(symbol);
        inner.removeFromLeft(5);
        g.setFont(font(11.5f, true));
        g.drawText(stopped ? "START" : "STOP", inner, juce::Justification::centredLeft);
    }
};

// The master section along the bottom of the deck: the Tape Stop key, the patch bay with a jack for each
// modulation source, then input and output trims, stereo width, the global pitch shift (semitones and cents on one
// display), the generations of tape copying and the dry/wet mix, shown as glowing digital readouts like the display
// of a video recorder. Drag a readout up or down (or turn the wheel over it) to change it; double-click returns it to
// its default. On the pitch display the left part sets semitones and the right part cents.
class MasterStrip final : public juce::Component
{
public:
    static constexpr size_t jackCount = 7;
    static constexpr std::array<const char*, jackCount> jackNames { "LFO 1", "LFO 2", "MACRO", "ENV", "SEQ", "X", "Y" };
    static constexpr std::array<const char*, jackCount> jackIds { "lfo_1_jack", "lfo_2_jack", "macro_jack", "env_jack",
                                                                  "seq_jack", "xy_x_jack", "xy_y_jack" };

    explicit MasterStrip(juce::AudioProcessorValueTreeState& state)
    {
        setComponentID("master_strip");
        setTitle("Master controls");
        stopKey.setTitle("Tape Stop");
        addAndMakeVisible(stopKey);
        stopAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, "transport_stop", stopKey);
        for (size_t i = 0; i < jackMarkers.size(); ++i)
        {
            // Empty markers that only hold each socket's place; the patch cable layer handles the mouse.
            jackMarkers[i].setComponentID(jackIds[i]);
            jackMarkers[i].setInterceptsMouseClicks(false, false);
            addAndMakeVisible(jackMarkers[i]);
        }
        for (size_t i = 0; i < displays.size(); ++i)
        {
            const auto& entry = entries[i];
            auto& display = displays[i];
            const auto id = juce::String(entry.id);
            auto* parameter = state.getParameter(id);
            jassert(parameter != nullptr);
            display.entry = &entry;
            display.seed = static_cast<int>(i);
            display.setComponentID("master_" + id);
            display.setTitle(parameter->getName(100));
            display.setSliderStyle(juce::Slider::RotaryVerticalDrag);
            display.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            display.setMouseDragSensitivity(220);
            display.setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            addAndMakeVisible(display);
            display.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state, id, display);
            display.setDoubleClickReturnValue(true, parameter->convertFrom0to1(parameter->getDefaultValue()));
        }
    }

    // Where each patch jack's socket is, in this component's coordinates.
    juce::Rectangle<float> jack(size_t index) const { return jackMarkers[index].getBounds().toFloat(); }

    // A jack's whole tile in the patch bay (name, lamp and socket): clicking anywhere on it opens the source.
    juce::Rectangle<float> jackTile(size_t index) const
    {
        const auto bay = jackArea.toFloat();
        return juce::Rectangle<float>(share(), bay.getHeight()).withCentre({ jack(index).getCentreX(), bay.getCentreY() });
    }

    // The sources' activity for the lamps above the jacks.
    void setActivity(const ModulationActivity& now)
    {
        activity = now;
        repaint(jackArea.withTrimmedBottom(18));
    }

    // Highlights the jack whose panel is open in the MOD drawer (-1 for none).
    void setOpenSource(int source)
    {
        if (std::exchange(openSource, source) != source)
            repaint(jackArea);
    }

    void paint(juce::Graphics& g) override
    {
        // Patch bay: a recessed plate with a socket per source, its name and a small sign of what it is doing.
        const auto bay = jackArea.toFloat();
        g.setColour(juce::Colours::black.withAlpha(.45f));
        g.fillRoundedRectangle(bay, 5);
        g.setColour(juce::Colours::white.withAlpha(.07f));
        g.drawRoundedRectangle(bay.translated(0, 1).reduced(.5f), 5, 1);
        for (size_t i = 0; i < jackNames.size(); ++i)
        {
            const auto socket = jack(i);
            const auto column = juce::Rectangle<float>(share(), bay.getHeight()).withCentre({ socket.getCentreX(), bay.getCentreY() });
            if (static_cast<int>(i) == openSource)
            {
                g.setColour(juce::Colour(0xff2a3a44).withAlpha(.9f));
                g.fillRoundedRectangle(column.reduced(1.5f, 1.5f), 4);
            }
            g.setFont(font(9.f, true));
            g.setColour(cream.withAlpha(.65f));
            g.drawText(jackNames[i], column.withHeight(11).translated(0, 1), juce::Justification::centred);
            paintIndicator(g, i, column.withY(bay.getY() + 12).withHeight(7).reduced(8, 0));
            juce::ColourGradient nut(juce::Colour(0xffd9dde1), socket.getX(), socket.getY(),
                                     juce::Colour(0xff55595e), socket.getRight(), socket.getBottom(), false);
            g.setGradientFill(nut);
            g.fillEllipse(socket);
            g.setColour(juce::Colours::black.withAlpha(.7f));
            g.drawEllipse(socket, .8f);
            g.setColour(juce::Colour(0xff060708));
            g.fillEllipse(socket.reduced(4.f));
        }
    }

    void resized() override
    {
        auto strip = getLocalBounds();
        stopKey.setBounds(strip.removeFromLeft(72).reduced(0, 2));
        strip.removeFromLeft(7);
        jackArea = strip.removeFromLeft(42 * static_cast<int>(jackCount) + 10);
        for (size_t i = 0; i < jackMarkers.size(); ++i)
            jackMarkers[i].setBounds(juce::Rectangle<float>(14, 14).withCentre(
                { static_cast<float>(jackArea.getX()) + 5.f + share() * (static_cast<float>(i) + .5f),
                  static_cast<float>(jackArea.getBottom()) - 9.f }).toNearestInt());

        // The readouts share what is left equally, one per control (semitones and cents each have their own).
        const auto area = strip.withTrimmedLeft(7);
        constexpr int gap = 7;
        const auto count = static_cast<int>(displays.size());
        const auto width = static_cast<float>(area.getWidth() - gap * (count - 1)) / static_cast<float>(count);
        auto x = static_cast<float>(area.getX());
        for (auto& display : displays)
        {
            display.setBounds(juce::Rectangle<float>(x, static_cast<float>(area.getY()), width,
                                                     static_cast<float>(area.getHeight())).toNearestInt());
            x += width + gap;
        }
    }

private:
    float share() const { return static_cast<float>(jackArea.getWidth() - 10) / static_cast<float>(jackCount); }

    void paintIndicator(juce::Graphics& g, size_t source, juce::Rectangle<float> area) const
    {
        static constexpr std::array<juce::uint32, jackCount> colours { 0xffe0473c, 0xfff0b030, 0xff3c98e6, 0xff4fd18b,
                                                                       0xffb07cf0, 0xffe6c15a, 0xff8ff5c4 };
        const juce::Colour colour(colours[source]);
        switch (source)
        {
            case 5:
            case 6:
            {
                // A bar from the middle, showing which way the pad leans on this axis.
                const auto value = source == 5 ? activity.x : activity.y;
                const auto bar = area.withSizeKeepingCentre(area.getWidth(), 4);
                g.setColour(juce::Colour(0xff11161b));
                g.fillRoundedRectangle(bar, 2);
                const auto mid = bar.getCentreX(), end = mid + value * bar.getWidth() * .5f;
                g.setColour(colour);
                g.fillRoundedRectangle(juce::Rectangle<float>(juce::jmin(mid, end), bar.getY(), juce::jmax(1.5f, std::abs(end - mid)),
                                                              bar.getHeight()), 2);
                break;
            }
            case 0:
            case 1:
            {
                // A lamp that pulses with the wave.
                const auto value = source == 0 ? activity.lfo1 : activity.lfo2;
                const auto lamp = juce::Rectangle<float>(6, 6).withCentre(area.getCentre());
                const auto level = .2f + .8f * (.5f + .5f * value);
                g.setColour(colour.withAlpha(.25f * level));
                g.fillEllipse(lamp.expanded(2));
                g.setColour(colour.withAlpha(level));
                g.fillEllipse(lamp);
                break;
            }
            case 2:
            {
                const auto bar = area.withSizeKeepingCentre(area.getWidth(), 4);
                g.setColour(juce::Colour(0xff11161b));
                g.fillRoundedRectangle(bar, 2);
                g.setColour(colour);
                g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * (.5f + .5f * activity.macro)), 2);
                break;
            }
            case 3:
            {
                // A small meter jumping with the envelope.
                constexpr int bars = 6;
                const auto lit = juce::roundToInt(juce::jlimit(0.f, 1.f, activity.envelope) * bars);
                const auto width = area.getWidth() / bars;
                for (int i = 0; i < bars; ++i)
                {
                    g.setColour(colour.withAlpha(i < lit ? .95f : .15f));
                    g.fillRect(area.getX() + width * static_cast<float>(i) + .5f, area.getY(), width - 1.f, area.getHeight());
                }
                break;
            }
            default:
            {
                // Eight dots, one lit for each pair of steps as the sequence runs.
                constexpr int dots = 8;
                const auto width = area.getWidth() / dots;
                for (int i = 0; i < dots; ++i)
                {
                    const auto on = i == (activity.step / 2) % dots;
                    g.setColour(colour.withAlpha(on ? 1.f : .2f));
                    g.fillEllipse(juce::Rectangle<float>(3, 3).withCentre({ area.getX() + width * (static_cast<float>(i) + .5f), area.getCentreY() }));
                }
                break;
            }
        }
    }

    struct Entry
    {
        const char* id;
        const char* caption;
        const char* unit;
        int decimals;
        bool signedValue;
    };
    static constexpr std::array<Entry, 7> entries {{
        { "input_gain", "In", "dB", 1, true }, { "output_gain", "Out", "dB", 1, true },
        { "global_width", "Width", "%", 0, false }, { "global_pitch", "Pitch", "st", 0, true },
        { "global_detune", "Tune", "ct", 0, true }, { "global_generations", "Gen", "x", 0, false },
        { "global_mix", "Mix", "%", 0, false }
    }};
    TransportKey stopKey;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> stopAttachment;
    juce::Rectangle<int> jackArea;
    std::array<juce::Component, jackCount> jackMarkers;
    ModulationActivity activity;
    int openSource = -1;

    class Display final : public juce::Slider
    {
    public:
        const Entry* entry = nullptr;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        // A second control whose value this display shows beside its own (cents beside semitones).
        Display* partner = nullptr;
        // Draws nothing itself: its value is shown by the display it sits on.
        bool silent = false;
        std::function<void()> onVisual;

        static juce::String format(const Entry& entry, double amount)
        {
            // juce::String(double, 0) prints full precision, so whole-number readouts are rounded to an int instead.
            const auto shown = std::abs(amount) < .5 * std::pow(10.0, -entry.decimals) ? 0.0 : amount;
            auto text = entry.decimals == 0 ? juce::String(juce::roundToInt(shown)) : juce::String(shown, entry.decimals);
            const auto zero = entry.decimals == 0 ? juce::String("0") : juce::String(0.0, entry.decimals);
            if (entry.signedValue && !text.startsWithChar('-') && text != zero)
                text = "+" + text;
            return text;
        }

        void paint(juce::Graphics& g) override
        {
            if (silent)
                return;
            const auto amount = getValue();
            auto text = format(*entry, amount);
            if (partner != nullptr)
                text << " " << format(*partner->entry, partner->getValue());
            const auto lit = isMouseOverOrDragging() || (partner != nullptr && partner->isMouseOverOrDragging());
            retro::vfdReadout(g, getLocalBounds(), entry->caption, text, entry->unit,
                              static_cast<float>(valueToProportionOfLength(amount)), lit, entry->signedValue,
                              brightness, bloom);
        }

        void valueChanged() override
        {
            if (onVisual)
                onVisual();
        }

        // Advances the tube's flicker to `now` (seconds). A steady tube shimmers very slightly; every few seconds
        // it stutters for a moment, and it strikes up with a burst of flashes when the window opens.
        void flicker(double now)
        {
            if (silent)
                return;
            if (nextStutter == 0)
            {
                // Each display strikes up a moment after the one before, then keeps its own rhythm.
                stutterStart = now + .08 * seed;
                stutterEnd = stutterStart + .45 + .1 * random.nextDouble();
                nextStutter = stutterEnd + 3.0 + 6.0 * random.nextDouble();
                phase = random.nextFloat() * juce::MathConstants<float>::twoPi;
            }
            float level;
            if (now < stutterStart)
                level = stutterEnd - stutterStart > .4 ? .1f : steady(now); // Dark until it strikes up.
            else if (now < stutterEnd)
            {
                // Uneven flashes, settling as the stutter runs out.
                const auto u = static_cast<float>((now - stutterStart) / (stutterEnd - stutterStart));
                level = random.nextFloat() < .45f + .4f * u ? steady(now) : .25f + .35f * random.nextFloat();
            }
            else
            {
                level = steady(now);
                if (now >= nextStutter)
                {
                    stutterStart = now;
                    stutterEnd = now + .12 + .22 * random.nextDouble();
                    nextStutter = stutterEnd + 3.5 + 7.0 * random.nextDouble();
                }
            }
            if (std::abs(level - brightness) > .004f)
            {
                brightness = level;
                repaint();
            }
        }

        int seed = 0;

        void mouseEnter(const juce::MouseEvent& e) override
        {
            juce::Slider::mouseEnter(e);
            silent && onVisual ? onVisual() : repaint();
        }

        void mouseExit(const juce::MouseEvent& e) override
        {
            juce::Slider::mouseExit(e);
            silent && onVisual ? onVisual() : repaint();
        }

    private:
        // A slow breath with a little mains hum on top.
        float steady(double now)
        {
            const auto t = static_cast<float>(now);
            return .95f + .03f * std::sin(t * 1.7f + phase) + .03f * (random.nextFloat() - .5f);
        }

        retro::VfdBloom bloom;
        juce::Random random;
        float brightness = 1.f, phase = 0;
        double stutterStart = 0, stutterEnd = 0, nextStutter = 0;
    };
    std::array<Display, entries.size()> displays;

    void tickFlicker()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        // About 30 changes a second is enough for the eye and keeps the repaints light.
        if (now - lastFlicker < 1.0 / 32)
            return;
        lastFlicker = now;
        for (auto& display : displays)
            display.flicker(now);
    }

    double lastFlicker = 0;
    juce::VBlankAttachment flickerFrames { this, [this] { tickFlicker(); } };
};

// A small phosphor screen that draws a source's recent values as a trace, newest on the right. Bipolar sources sit
// around a centre line; unipolar ones (the envelope) rise from the bottom.
class ScopeScreen final : public juce::Component
{
public:
    ScopeScreen(juce::Colour traceColour, bool centred) : colour(traceColour), bipolar(centred)
    {
        setInterceptsMouseClicks(false, false);
    }

    void push(float value)
    {
        history[static_cast<size_t>(head)] = value;
        head = (head + 1) % static_cast<int>(history.size());
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(.5f);
        g.setColour(juce::Colour(0xff060c0d));
        g.fillRoundedRectangle(area, 6);
        const auto graph = area.reduced(8, 8);
        g.setColour(colour.withAlpha(.14f));
        for (int i = 1; i < 8; ++i)
            g.drawVerticalLine(juce::roundToInt(graph.getX() + graph.getWidth() * static_cast<float>(i) / 8.f), graph.getY(), graph.getBottom());
        g.drawHorizontalLine(juce::roundToInt(bipolar ? graph.getCentreY() : graph.getBottom()), graph.getX(), graph.getRight());
        juce::Path trace;
        const auto count = static_cast<int>(history.size());
        for (int i = 0; i < count; ++i)
        {
            const auto value = juce::jlimit(-1.f, 1.f, history[static_cast<size_t>((head + i) % count)]);
            const auto x = graph.getX() + graph.getWidth() * static_cast<float>(i) / static_cast<float>(count - 1);
            const auto y = bipolar ? graph.getCentreY() - value * graph.getHeight() * .46f
                                   : graph.getBottom() - juce::jmax(0.f, value) * graph.getHeight();
            i == 0 ? trace.startNewSubPath(x, y) : trace.lineTo(x, y);
        }
        retro::crtTrace(g, trace, colour, 1.6f);
        retro::crtGlass(g, area, 6);
    }

private:
    juce::Colour colour;
    bool bipolar;
    std::array<float, 120> history {};
    int head = 0;
};

// The sequencer's sixteen steps as bars around a centre line. Drag across them to draw values, double-click a bar
// to return it to zero; steps past the sequence's length are dimmed and the playing step lights up.
class StepEditor final : public juce::Component
{
public:
    explicit StepEditor(juce::AudioProcessorValueTreeState& state) : length(state.getRawParameterValue("seq_length"))
    {
        setComponentID("seq_steps");
        setTitle("Sequencer steps");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        for (size_t i = 0; i < steps.size(); ++i)
            steps[i] = state.getParameter("seq_step_" + juce::String(static_cast<int>(i + 1)));
    }

    void setPlaying(int step)
    {
        if (std::exchange(playing, step) != step)
            repaint();
    }

    // Writes new values into every step at once (for Random and Clear).
    void fill(const std::function<float(int)>& value)
    {
        for (int i = 0; i < static_cast<int>(steps.size()); ++i)
            write(i, value(i));
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(.5f);
        g.setColour(juce::Colour(0xff0d0c1d));
        g.fillRoundedRectangle(area, 6);
        const auto graph = graphArea();
        g.setColour(juce::Colour(0xff5a55a6).withAlpha(.7f));
        g.drawHorizontalLine(juce::roundToInt(graph.getCentreY()), graph.getX(), graph.getRight());
        const auto active = juce::jlimit(1, 16, juce::roundToInt(length->load(std::memory_order_relaxed)));
        const juce::Colour bar(0xffb07cf0);
        for (int i = 0; i < static_cast<int>(steps.size()); ++i)
        {
            const auto column = cell(i).reduced(2.f, 0);
            const auto value = stepValue(i);
            const auto top = value >= 0 ? graph.getCentreY() - value * graph.getHeight() * .5f : graph.getCentreY();
            const auto height = juce::jmax(1.5f, std::abs(value) * graph.getHeight() * .5f);
            const auto lit = i == playing && i < active;
            const auto alpha = i < active ? 1.f : .25f;
            if (lit)
            {
                g.setColour(bar.withAlpha(.18f));
                g.fillRoundedRectangle(column.withY(graph.getY()).withHeight(graph.getHeight()), 2);
            }
            g.setColour((lit ? juce::Colour(0xffe7d6ff) : bar).withAlpha(alpha));
            g.fillRoundedRectangle(column.withY(top).withHeight(height), 2);
            if (i % 4 == 0)
            {
                g.setColour(cream.withAlpha(.35f * alpha));
                g.setFont(readoutFont(9.f));
                g.drawText(juce::String(i + 1), column.withY(area.getBottom() - 12).withHeight(10), juce::Justification::centred);
            }
        }
        retro::crtGlass(g, area, 6);
    }

    void mouseDown(const juce::MouseEvent& event) override { draw(event.position); }
    void mouseDrag(const juce::MouseEvent& event) override { draw(event.position); }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (const auto i = indexAt(event.position.x); i >= 0)
            write(i, 0.f);
    }

private:
    juce::Rectangle<float> graphArea() const { return getLocalBounds().toFloat().reduced(8.f, 10.f).withTrimmedBottom(8.f); }

    juce::Rectangle<float> cell(int index) const
    {
        const auto graph = graphArea();
        const auto width = graph.getWidth() / static_cast<float>(steps.size());
        return { graph.getX() + width * static_cast<float>(index), graph.getY(), width, graph.getHeight() };
    }

    int indexAt(float x) const
    {
        const auto graph = graphArea();
        const auto index = static_cast<int>((x - graph.getX()) / graph.getWidth() * static_cast<float>(steps.size()));
        return juce::isPositiveAndBelow(index, static_cast<int>(steps.size())) ? index : -1;
    }

    float stepValue(int index) const
    {
        const auto* p = steps[static_cast<size_t>(index)];
        return p != nullptr ? p->convertFrom0to1(p->getValue()) * .01f : 0.f;
    }

    void draw(juce::Point<float> at)
    {
        const auto index = indexAt(at.x);
        if (index < 0)
            return;
        const auto graph = graphArea();
        write(index, juce::jlimit(-1.f, 1.f, (graph.getCentreY() - at.y) / (graph.getHeight() * .5f)));
    }

    void write(int index, float value)
    {
        auto* p = steps[static_cast<size_t>(index)];
        if (p == nullptr)
            return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(std::round(value * 100.f)));
        p->endChangeGesture();
        repaint();
    }

    std::array<juce::RangedAudioParameter*, 16> steps {};
    const std::atomic<float>* length;
    int playing = -1;
};

// A small hardware key with a printed caption, for one-shot actions such as the sequencer's Random and Clear.
class PanelKey final : public juce::Button
{
public:
    explicit PanelKey(const juce::String& caption) : juce::Button(caption)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setMouseClickGrabsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto area = getLocalBounds().toFloat().reduced(1.f);
        g.setColour(juce::Colours::black.withAlpha(.55f));
        g.fillRoundedRectangle(area.translated(0, 2.f), 5);
        area = area.withTrimmedBottom(2.f).translated(0, down ? 1.5f : 0.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(over ? 0xff4a4e53 : 0xff3d4146), 0, area.getY(),
                                               juce::Colour(0xff1b1d20), 0, area.getBottom(), false));
        g.fillRoundedRectangle(area, 5);
        g.setColour(juce::Colours::white.withAlpha(.16f));
        g.drawHorizontalLine(juce::roundToInt(area.getY() + 1), area.getX() + 4, area.getRight() - 4);
        g.setColour(juce::Colours::black.withAlpha(.8f));
        g.drawRoundedRectangle(area, 5, 1);
        g.setFont(font(12.f, true));
        g.setColour(cream);
        g.drawText(getButtonText(), area, juce::Justification::centred);
    }
};

// The MOD drawer: slides up out of the master section over the cartridge shelf and holds the settings of the
// modulation sources, one tab each (LFO 1, LFO 2, Macro, ENV, SEQ). Clicking a jack opens it on that source's tab;
// clicking the same jack again, or Hide, slides it away.
// The XY pad in the MOD drawer: one handle moving the X and Y macros together, like the Grain card's screen.
// Each axis lists the knobs it is patched to.
class XyPad final : public juce::Component
{
public:
    explicit XyPad(juce::AudioProcessorValueTreeState& parameters)
        : state(parameters), padX(parameters.getParameter("xy_x_amount")), padY(parameters.getParameter("xy_y_amount"))
    {
        setComponentID("xy_pad");
        setTitle("XY pad: drag to move the X and Y macros, double-click to centre");
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragging = true;
        padX->beginChangeGesture();
        padY->beginChangeGesture();
        moveTo(event.position);
    }
    void mouseDrag(const juce::MouseEvent& event) override { moveTo(event.position); }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (!std::exchange(dragging, false))
            return;
        padX->endChangeGesture();
        padY->endChangeGesture();
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        for (auto* p : { padX, padY })
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->getDefaultValue());
            p->endChangeGesture();
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced(.5f);
        g.setColour(juce::Colour(0xff06120d));
        g.fillRoundedRectangle(area, 6);
        g.setColour(juce::Colours::black.withAlpha(.7f));
        g.drawRoundedRectangle(area, 6, 1);
        const auto pad = padArea();
        for (int i = 1; i < 8; ++i)
        {
            const auto t = static_cast<float>(i) / 8.f;
            g.setColour(mint.withAlpha(i == 4 ? .22f : .08f));
            g.drawVerticalLine(juce::roundToInt(pad.getX() + pad.getWidth() * t), pad.getY(), pad.getBottom());
            g.drawHorizontalLine(juce::roundToInt(pad.getY() + pad.getHeight() * t), pad.getX(), pad.getRight());
        }
        g.setColour(mint.withAlpha(.25f));
        g.drawRect(pad, 1.f);

        const juce::Point<float> at { pad.getX() + padX->getValue() * pad.getWidth(),
                                      pad.getBottom() - padY->getValue() * pad.getHeight() };
        g.setColour(gold.withAlpha(.18f));
        g.drawVerticalLine(juce::roundToInt(at.x), pad.getY(), pad.getBottom());
        g.setColour(mint.withAlpha(.18f));
        g.drawHorizontalLine(juce::roundToInt(at.y), pad.getX(), pad.getRight());
        const auto radius = dragging ? 8.f : 6.5f;
        g.setColour(gold.withAlpha(.2f));
        g.fillEllipse(juce::Rectangle<float>(4 * radius, 4 * radius).withCentre(at));
        g.setColour(gold);
        g.drawEllipse(juce::Rectangle<float>(2 * radius, 2 * radius).withCentre(at), 1.6f);

        // What each axis drives, beside the pad: the lines are gathered first so the block can sit in the middle of
        // the free space, left-aligned within itself.
        struct Line
        {
            juce::String text;
            juce::Colour colour;
            float size;
            int indent, height;
        };
        std::vector<Line> lines;
        for (const auto& [axis, prefix, colour, parameter] : { std::tuple { "X", "xy_x_", gold, padX },
                                                                std::tuple { "Y", "xy_y_", mint, padY } })
        {
            if (!lines.empty())
                lines.push_back({ {}, {}, 0.f, 0, 10 });
            lines.push_back({ juce::String(axis) + "  " + juce::String(juce::roundToInt(parameter->getValue() * 200.f - 100.f)),
                              colour, 14.f, 0, 19 });
            auto any = false;
            for (int slot = 1; slot <= 4; ++slot)
            {
                const auto suffix = juce::String(slot);
                auto* target = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter(prefix + juce::String("target_") + suffix));
                const auto* active = state.getRawParameterValue(prefix + juce::String("active_") + suffix);
                const auto depth = state.getRawParameterValue(prefix + juce::String("depth_") + suffix)->load();
                if (target == nullptr || target->getIndex() == 0 || active->load() < .5f || depth == 0)
                    continue;
                any = true;
                lines.push_back({ target->getCurrentChoiceName() + "  " + (depth > 0 ? "+" : "")
                                      + juce::String(juce::roundToInt(depth)) + "%",
                                  cream.withAlpha(.7f), 12.5f, 12, 16 });
            }
            if (!any)
                lines.push_back({ "patch from the " + juce::String(axis) + " jack", cream.withAlpha(.4f), 12.5f, 12, 16 });
        }
        const auto free = area.withTrimmedLeft(pad.getRight() - area.getX()).reduced(8, 10);
        float blockWidth = 0, blockHeight = 0;
        for (const auto& line : lines)
        {
            if (line.size > 0)
                blockWidth = juce::jmax(blockWidth, static_cast<float>(line.indent)
                                                        + juce::GlyphArrangement::getStringWidth(readoutFont(line.size), line.text));
            blockHeight += static_cast<float>(line.height);
        }
        blockWidth = juce::jmin(blockWidth + 2.f, free.getWidth());
        auto text = juce::Rectangle<float>(blockWidth, juce::jmin(blockHeight, free.getHeight())).withCentre(free.getCentre());
        for (const auto& line : lines)
        {
            const auto row = text.removeFromTop(static_cast<float>(line.height));
            if (line.size <= 0)
                continue;
            g.setFont(readoutFont(line.size));
            g.setColour(line.colour);
            g.drawText(line.text, row.withTrimmedLeft(static_cast<float>(line.indent)), juce::Justification::centredLeft);
        }
        retro::crtGlass(g, area, 6);
    }

private:
    juce::Rectangle<float> padArea() const
    {
        const auto area = getLocalBounds().toFloat().reduced(10);
        const auto side = area.getHeight();
        return area.withWidth(juce::jmin(side * 1.35f, area.getWidth() * .55f));
    }

    void moveTo(juce::Point<float> point)
    {
        const auto pad = padArea();
        padX->setValueNotifyingHost(juce::jlimit(0.f, 1.f, (point.x - pad.getX()) / pad.getWidth()));
        padY->setValueNotifyingHost(juce::jlimit(0.f, 1.f, (pad.getBottom() - point.y) / pad.getHeight()));
        repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter* padX;
    juce::RangedAudioParameter* padY;
    const juce::Colour mint { 0xff8ff5c4 }, gold { 0xffe6c15a };
    bool dragging = false;
    juce::VBlankAttachment frames { this, [this] { if (isShowing()) repaint(); } };
};

class ModDrawer final : public juce::Component
{
public:
    static constexpr int height = 252;
    static constexpr int pages = 7;
    static constexpr std::array<const char*, pages> names { "LFO 1", "LFO 2", "MACRO", "ENV", "SEQ", "X", "Y" };
    static constexpr std::array<juce::uint32, pages> colours { 0xffe0473c, 0xfff0b030, 0xff3c98e6, 0xff4fd18b, 0xffb07cf0,
                                                               0xffe6c15a, 0xff8ff5c4 };

    explicit ModDrawer(juce::AudioProcessorValueTreeState& state)
        : steps(state), pad(state), syncs { state.getRawParameterValue("lfo_1_sync"), state.getRawParameterValue("lfo_2_sync") }
    {
        setComponentID("mod_drawer");
        setTitle("Modulation drawer");
        for (size_t i = 0; i < screens.size(); ++i)
        {
            screens[i] = std::make_unique<ScopeScreen>(juce::Colour(colours[i]), i != 3);
            addChildComponent(*screens[i]);
        }
        addChildComponent(steps);
        addChildComponent(pad);
        const std::array<std::vector<const char*>, pages> controls {{
            { "lfo_1_active", "lfo_1_shape", "lfo_1_rate", "lfo_1_sync", "lfo_1_division" },
            { "lfo_2_active", "lfo_2_shape", "lfo_2_rate", "lfo_2_sync", "lfo_2_division" },
            { "macro_amount" },
            { "env_source", "env_attack", "env_release", "env_gain" },
            { "seq_division", "seq_length", "seq_glide", "seq_swing" },
            { "xy_x_amount", "xy_y_amount" },
            { "xy_x_amount", "xy_y_amount" }
        }};
        for (size_t page = 0; page < controls.size(); ++page)
            for (const auto* id : controls[page])
                if (auto* parameter = state.getParameter(id))
                {
                    // The X and Y pages share one pair of knobs.
                    if (byId.count(id) != 0)
                        continue;
                    auto name = parameter->getName(100).fromFirstOccurrenceOf(" ", false, false);
                    if (juce::String(id).startsWith("lfo_"))
                        name = name.fromFirstOccurrenceOf(" ", false, false);
                    // Full-size controls in the Tape card's finish: black skirted knobs with aluminium caps,
                    // slide switches and wide selectors.
                    auto tile = std::make_unique<ParameterTile>(state, *parameter, name, 0, cream, false);
                    addChildComponent(*tile);
                    byId[id] = tile.get();
                    tiles[page].push_back(std::move(tile));
                }
        for (auto* button : { &random, &clear })
            addChildComponent(*button);
        random.setComponentID("seq_random");
        clear.setComponentID("seq_clear");
        random.onClick = [this]
        {
            juce::Random dice;
            steps.fill([&dice](int) { return dice.nextFloat() * 2.f - 1.f; });
        };
        clear.onClick = [this] { steps.fill([](int) { return 0.f; }); };
        setPaintingIsUnclipped(true);
        select(0);
    }

    // Tells the surface which tab was chosen or that the drawer should close.
    std::function<void(int source)> onChoose;
    std::function<void()> onHide;

    int page() const { return current; }

    void select(int source)
    {
        current = juce::jlimit(0, pages - 1, source);
        const auto xy = current >= 5;
        for (size_t i = 0; i < screens.size(); ++i)
            screens[i]->setVisible(static_cast<int>(i) == current);
        steps.setVisible(current == 4);
        pad.setVisible(xy);
        random.setVisible(current == 4);
        clear.setVisible(current == 4);
        for (size_t p = 0; p < tiles.size(); ++p)
            for (auto& tile : tiles[p])
                tile->setVisible(static_cast<int>(p) == (xy ? 5 : current));
        resized();
        repaint();
    }

    void update(const ModulationActivity& now)
    {
        // A synced LFO follows its Division and ignores Rate, and the other way round: the unused one dims.
        for (const auto* lfo : { "lfo_1_", "lfo_2_" })
        {
            const auto prefix = juce::String(lfo);
            if (auto* sync = byId[prefix + "sync"]; sync != nullptr)
            {
                const auto synced = syncs[prefix == "lfo_1_" ? 0 : 1]->load(std::memory_order_relaxed) >= .5f;
                if (auto* rate = byId[prefix + "rate"])
                    rate->setAlpha(synced ? .35f : 1.f);
                if (auto* division = byId[prefix + "division"])
                    division->setAlpha(synced ? 1.f : .35f);
            }
        }
        screens[0]->push(now.lfo1);
        screens[1]->push(now.lfo2);
        screens[2]->push(now.macro);
        screens[3]->push(now.envelope);
        steps.setPlaying(now.step);
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        // A dark metal tray hanging from under the master section, rounded at the bottom, with a pull grip.
        juce::Path tray;
        tray.addRoundedRectangle(area.getX(), area.getY() - 12.f, area.getWidth(), area.getHeight() + 12.f - 2.f, 10.f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2b3035), 0, area.getY(), juce::Colour(0xff1a1d21), 0, area.getBottom(), false));
        g.fillPath(tray);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(.6f), 0, area.getY(),
                                               juce::Colours::transparentBlack, 0, area.getY() + 14, false));
        g.fillRect(area.withHeight(14));
        g.setColour(juce::Colours::black.withAlpha(.8f));
        g.strokePath(tray, juce::PathStrokeType(1.f));
        g.setColour(juce::Colour(0xff4a5056));
        g.fillRoundedRectangle(juce::Rectangle<float>(60, 4).withCentre({ area.getCentreX(), area.getBottom() - 9 }), 2);

        // Tabs, each with its source's colour, and Hide on the right.
        for (int i = 0; i < pages; ++i)
        {
            const auto tab = tabArea(i).toFloat();
            const auto on = i == current;
            g.setColour(on ? juce::Colour(0xff363c42) : juce::Colour(0xff1b1e22));
            g.fillRoundedRectangle(tab, 5);
            g.setColour(juce::Colours::black.withAlpha(.7f));
            g.drawRoundedRectangle(tab.reduced(.5f), 5, 1);
            g.setColour(juce::Colour(colours[static_cast<size_t>(i)]).withAlpha(on ? 1.f : .5f));
            g.fillEllipse(juce::Rectangle<float>(7, 7).withCentre({ tab.getX() + 12, tab.getCentreY() }));
            g.setFont(font(12.f, true));
            g.setColour(on ? cream : cream.withAlpha(.5f));
            g.drawText(names[static_cast<size_t>(i)], tab.withTrimmedLeft(20), juce::Justification::centredLeft);
        }
        g.setFont(font(12.f, true));
        g.setColour(cream.withAlpha(.6f));
        g.drawText(juce::String("Hide ") + juce::String::charToString(0x25b4), hideArea(), juce::Justification::centredRight);
        // The controls sit on a recessed plate, split into the groups the layout below uses.
        const auto plate = controlArea().toFloat();
        g.setColour(juce::Colours::black.withAlpha(.35f));
        g.fillRoundedRectangle(plate, 7);
        g.setColour(juce::Colours::white.withAlpha(.06f));
        g.drawRoundedRectangle(plate.translated(0, 1).reduced(.5f), 7, 1);
        for (const auto& divider : dividers())
        {
            g.setColour(juce::Colours::black.withAlpha(.5f));
            g.drawVerticalLine(divider, plate.getY() + 12, plate.getBottom() - 12);
            g.setColour(juce::Colours::white.withAlpha(.05f));
            g.drawVerticalLine(divider + 1, plate.getY() + 12, plate.getBottom() - 12);
        }
        if (current == 2)
        {
            g.setFont(font(12.5f));
            g.setColour(cream.withAlpha(.65f));
            g.drawFittedText("Turn the Macro, or automate it, to move every knob patched from the MACRO jack at once.",
                             plate.withTrimmedLeft(170).reduced(14, 30).toNearestInt(), juce::Justification::centredLeft, 4);
        }
        if (current >= 5)
        {
            g.setFont(font(12.5f));
            g.setColour(cream.withAlpha(.65f));
            g.drawFittedText("Drag on the pad, or on a Grain card's screen, to move both at once. Patch knobs from the "
                             "X and Y jacks; the middle of the pad leaves them where they are.",
                             plate.withTrimmedLeft(300).reduced(12, 26).toNearestInt(), juce::Justification::centredLeft, 5);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!event.mouseWasClicked())
            return;
        const auto at = event.getPosition();
        if (hideArea().contains(at))
        {
            if (onHide)
                onHide();
            return;
        }
        for (int i = 0; i < pages; ++i)
            if (tabArea(i).contains(at))
            {
                select(i);
                if (onChoose)
                    onChoose(i);
            }
    }

    void resized() override
    {
        const auto content = contentArea();
        const auto controls = controlArea();
        const auto screen = content.withRight(controls.getX() - 12);
        for (auto& s : screens)
            s->setBounds(screen);
        steps.setBounds(screen);
        pad.setBounds(screen);
        // Each source's controls are laid out on the plate by hand: the main knob large on the left, switches and
        // selectors stacked beside it.
        const auto plate = controls.reduced(10, 6);
        const auto set = [this](const char* id, juce::Rectangle<int> bounds, bool hero = false)
        {
            if (auto* tile = byId[id])
            {
                tile->setHero(hero);
                tile->setBounds(bounds);
            }
        };
        auto left = plate.withWidth(150);
        auto right = plate.withTrimmedLeft(162);
        // A plain knob tile only as tall as caption, knob and readout need, centred on the plate, so the three sit
        // close together instead of spreading over the full height.
        const auto knob = [&plate](juce::Rectangle<int> column)
        {
            return column.withY(plate.getY()).withHeight(plate.getHeight()).withSizeKeepingCentre(column.getWidth(), 106);
        };
        // The same for a large knob: room for its 70 px cap and printed scale, and no more.
        const auto heroKnob = [&plate](juce::Rectangle<int> column)
        {
            return column.withY(plate.getY()).withHeight(plate.getHeight()).withSizeKeepingCentre(column.getWidth(), 132);
        };
        switch (current)
        {
            case 0:
            case 1:
            {
                const juce::String prefix = current == 0 ? "lfo_1_" : "lfo_2_";
                set((prefix + "rate").toRawUTF8(), heroKnob(left), true);
                auto top = right.removeFromTop(right.getHeight() / 2);
                set((prefix + "shape").toRawUTF8(), top.removeFromLeft(top.getWidth() / 2).withTrimmedTop(14));
                set((prefix + "division").toRawUTF8(), top.withTrimmedTop(14));
                set((prefix + "active").toRawUTF8(), right.removeFromLeft(right.getWidth() / 2).withTrimmedTop(6));
                set((prefix + "sync").toRawUTF8(), right.withTrimmedTop(6));
                break;
            }
            case 2:
                set("macro_amount", heroKnob(left), true);
                break;
            case 5:
            case 6:
                // Two matching knobs, the same size on both pages.
                set("xy_x_amount", knob(left.withWidth(140)));
                set("xy_y_amount", knob(left.withX(left.getX() + 140).withWidth(140)));
                break;
            case 3:
            {
                set("env_source", left.withTrimmedTop(30).withHeight(80));
                const auto third = right.getWidth() / 3;
                set("env_attack", knob(right.removeFromLeft(third)));
                set("env_release", knob(right.removeFromLeft(third)));
                set("env_gain", knob(right));
                break;
            }
            default:
            {
                auto column = left;
                set("seq_division", column.removeFromTop(70).withTrimmedTop(10));
                column.removeFromTop(16);
                random.setBounds(column.removeFromTop(30).reduced(6, 0));
                column.removeFromTop(8);
                clear.setBounds(column.removeFromTop(30).reduced(6, 0));
                const auto third = right.getWidth() / 3;
                set("seq_length", knob(right.removeFromLeft(third)));
                set("seq_glide", knob(right.removeFromLeft(third)));
                set("seq_swing", knob(right));
                break;
            }
        }
    }

    // Where the plate's group dividers fall for the open page, in this component's coordinates.
    std::vector<int> dividers() const
    {
        if (current == 2 || current >= 5)
            return {};
        return { controlArea().reduced(10, 6).getX() + 156 };
    }

    juce::Button& randomButton() { return random; }

private:
    juce::Rectangle<int> tabArea(int i) const { return { 14 + i * 84, 12, 78, 24 }; }
    juce::Rectangle<int> hideArea() const { return { getWidth() - 110, 12, 96, 24 }; }
    juce::Rectangle<int> contentArea() const { return getLocalBounds().withTrimmedTop(46).reduced(14, 0).withTrimmedBottom(20); }
    juce::Rectangle<int> controlArea() const { return contentArea().removeFromRight(470); }

    std::array<std::unique_ptr<ScopeScreen>, 4> screens;
    StepEditor steps;
    XyPad pad;
    std::array<std::vector<std::unique_ptr<ParameterTile>>, pages> tiles;
    std::map<juce::String, ParameterTile*> byId;
    std::array<const std::atomic<float>*, 2> syncs;
    PanelKey random { "Random" }, clear { "Clear" };
    int current = 0;
};

// The opening the MOD drawer slides through, below the master section: it clips the drawer so it seems to drop
// out from under the strip. Only the drawer itself takes the mouse.
class ModDrawerWindow final : public juce::Component
{
public:
    explicit ModDrawerWindow(juce::AudioProcessorValueTreeState& state) : drawer(state)
    {
        setInterceptsMouseClicks(false, true);
        addChildComponent(drawer);
    }

    ModDrawer drawer;
    // Called every frame with how far the drawer is out (0 to 1), so the window can grow and shrink with it.
    std::function<void(float shown)> onShown;

    bool isOpen() const { return target > .5f; }

    void open(int source)
    {
        drawer.select(source);
        setOpen(true);
    }

    void setOpen(bool shouldOpen)
    {
        target = shouldOpen ? 1.f : 0.f;
        if (shouldOpen)
            drawer.setVisible(true);
        started = juce::Time::getMillisecondCounterHiRes();
        from = amount;
        if (getPeer() == nullptr)
            amount = target; // No frames without a window: show the finished state.
        place();
    }

    void resized() override { place(); }

private:
    void place()
    {
        // Out quickly and settling (ease out), back in gathering speed (ease in). The drawer's bottom edge
        // and the window's grow together, so the whole drawer slides out as one piece.
        const auto opening = target > from;
        const auto eased = opening ? 1.f - (1.f - amount) * (1.f - amount) * (1.f - amount) : amount * amount;
        const auto height = static_cast<float>(ModDrawer::height);
        drawer.setBounds(0, juce::roundToInt(height * eased - height), getWidth(), ModDrawer::height);
        if (amount <= 0.f && target <= 0.f)
            drawer.setVisible(false);
        if (onShown)
            onShown(eased);
    }

    void tick()
    {
        if (amount == target)
            return;
        const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((juce::Time::getMillisecondCounterHiRes() - started) * .001 / .36));
        amount = from + (target - from) * u;
        if (u >= 1.f)
            amount = target;
        place();
    }

    float amount = 0, target = 0, from = 0;
    double started = 0;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// Patch cables from the master section's jacks to the knobs on the cards. Each cable is one of the four routes of
// LFO 1, LFO 2 or the Macro, so it is saved, recalled and automated like any other setting. Drag from a jack to a
// knob to patch it; drag a plug to another knob to move it, or off into the open to pull it out. Over a plug the
// wheel (or Alt-drag) sets how deep it modulates, a right-click offers depths and removal, and a double-click
// pulls it. Clicking a jack opens that source's panel. The cables hang on a simple rope model and swing as the
// rack scrolls under them.
class PatchCables final : public juce::Component
{
public:
    struct Host
    {
        // Every knob in the rack for a parameter: both copies of an effect when two are seated.
        std::function<std::vector<juce::Slider*>(const juce::String& id)> findKnobs;
        // Where a seated card keeps its key to the full controls, for knobs that live only in that panel.
        std::function<std::optional<juce::Point<float>>(const juce::String& id)> findControlsKey;
        // The knob for a parameter in the open controls window, and that window's plate (empty when none is open).
        std::function<juce::Slider*(const juce::String& id)> findPanelKnob;
        std::function<juce::Rectangle<float>()> panelPlate;
        // Lifts the cables over the controls window while one is carried, and lets them back under it after.
        std::function<void(bool carrying)> raise;
        // The card whose input socket is under a point (its instance), or -1; and opening that card's controls.
        std::function<int(juce::Point<float>)> controlsKeyAt;
        std::function<void(int instance)> openControls;
        // While a card's controls are open: its input socket on the card and the window's IN socket.
        std::function<std::optional<std::pair<juce::Point<float>, juce::Point<float>>>()> panelInputs;
        std::function<juce::Rectangle<float>()> rackWindow;
        std::function<juce::Point<float>(size_t source)> jack;
        std::function<juce::Rectangle<float>(size_t source)> jackTile;
        std::function<void(size_t source)> openSource;
        std::function<void(const juce::String& heading, const juce::String& text)> notify;
    };

    PatchCables(juce::AudioProcessorValueTreeState& parameters, Host callbacks)
        : state(parameters), host(std::move(callbacks))
    {
        setComponentID("patch_cables");
        setTitle("Modulation patch cables");
        setPaintingIsUnclipped(true);
    }

    static constexpr size_t sourceCount = 7, routesPerSource = 4;

    // The Macro and the XY pad's two macros have a switch on every route.
    static bool isMacro(size_t source) { return source == 2 || source >= 5; }

    bool hitTest(int x, int y) override
    {
        const juce::Point<float> at(static_cast<float>(x), static_cast<float>(y));
        if (carrying.has_value())
            return true;
        // On the open controls window only the window's own controls take the mouse.
        if (plate().contains(at))
            return false;
        return jackAt(at).has_value() || plugAt(at).has_value();
    }

    juce::Rectangle<float> plate() const { return host.panelPlate ? host.panelPlate() : juce::Rectangle<float>(); }

    // Knobs a carried cable can go into: those in the rack, and those in the open controls window.
    std::vector<juce::Slider*> dropKnobs(const juce::String& id) const
    {
        auto knobs = host.findKnobs(id);
        if (host.findPanelKnob)
            if (auto* knob = host.findPanelKnob(id))
                knobs.push_back(knob);
        return knobs;
    }

    // Whether a knob's input can take a cable here: in view in the rack, or on the open window's plate.
    bool reachable(juce::Slider& knob) const
    {
        const auto socket = socketOf(knob);
        const auto onPlate = plate().contains(socket);
        const auto inPanel = host.findPanelKnob && host.findPanelKnob(knob.getComponentID().startsWith("cart2_")
                                 ? knob.getComponentID().substring(6) : knob.getComponentID()) == &knob;
        return inPanel ? onPlate : host.rackWindow().contains(socket) && !onPlate;
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        // A hand over a jack's tile or a plug shows they can be clicked and dragged.
        const auto jack = jackAt(event.position);
        const auto over = jack.has_value() || plugAt(event.position).has_value();
        setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        std::optional<juce::Point<float>> dock;
        for (const auto& cable : cables)
            if (cable.docked && !cable.offscreen && (cable.to.getDistanceFrom(event.position) < 10.f
                    ))
                dock = cable.to;
        if (std::exchange(hoveredJack, jack) != jack | std::exchange(hoveredDock, dock) != dock)
            repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (std::exchange(hoveredJack, std::nullopt).has_value() | std::exchange(hoveredDock, std::nullopt).has_value())
            repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        pressedAt = event.position;
        moved = false;
        if (const auto plug = plugAt(event.position))
        {
            if (event.mods.isPopupMenu())
            {
                showPlugMenu(*plug);
                return;
            }
            carrying = *plug;
            tuning = event.mods.isAltDown();
            tuneStart = depth(*plug);
            if (!tuning)
                carryingNew = false;
            return;
        }
        if (const auto source = jackAt(event.position))
        {
            pressedJack = *source;
            const auto slot = freeSlot(*source);
            if (!slot)
                return;
            carrying = Key { *source, *slot };
            carryingNew = true;
            tuning = false;
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (event.getDistanceFromDragStart() > 4)
            moved = true;
        if (!carrying)
        {
            if (moved && pressedJack)
                host.notify("All routes in use", "Each source can drive four knobs. Pull a plug out first.");
            pressedJack.reset();
            return;
        }
        if (tuning)
        {
            setDepth(*carrying, tuneStart - (event.position.y - pressedAt.y) * .8f);
            return;
        }
        hand = event.position;
        hover = knobAt(event.position);
        if (moved && !raised && host.raise)
        {
            raised = true;
            host.raise(true);
        }
        const auto key = host.controlsKeyAt ? host.controlsKeyAt(hand) : -1;
        if (key != restingKey)
        {
            restingKey = key;
            restingSince = juce::Time::getMillisecondCounterHiRes();
        }
        openRestingKey();
        wake();
    }

    // A cable held on a card's input for a moment opens that card's controls, so it can go into one of them.
    void openRestingKey()
    {
        if (!carrying || tuning || restingKey < 0 || restingKey == openedKey || !host.openControls)
            return;
        if (juce::Time::getMillisecondCounterHiRes() - restingSince < 300.0)
            return;
        openedKey = restingKey;
        host.openControls(restingKey);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        const auto wasCarrying = carrying;
        const auto wasNew = carryingNew;
        carrying.reset();
        hover = {};
        restingKey = openedKey = -1;
        if (std::exchange(raised, false) && host.raise)
            host.raise(false);
        if (!moved)
        {
            if (pressedJack && host.openSource)
                host.openSource(*pressedJack);
            pressedJack.reset();
            wake();
            return;
        }
        pressedJack.reset();
        if (!wasCarrying || tuning)
        {
            tuning = false;
            return;
        }
        if (const auto target = knobAt(event.position))
        {
            for (auto* knob : host.findKnobs(targetKnobs[static_cast<size_t>(target->first)]))
                if (knobBounds(*knob) == target->second)
                    droppedOn[index(*wasCarrying)] = knob;
            connect(*wasCarrying, target->first, wasNew ? 50.f : depth(*wasCarrying));
            // The new plug drops in from where the hand let go.
            ropes[index(*wasCarrying)].reset();
            ropes[index(*wasCarrying)].dropFrom = event.position;
        }
        else if (!wasNew)
            disconnect(*wasCarrying);
        wake();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (const auto plug = plugAt(event.position))
            disconnect(*plug);
        wake();
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto plug = plugAt(event.position))
            setDepth(*plug, depth(*plug) + wheel.deltaY * 40.f);
    }

    void paint(juce::Graphics& g) override
    {
        // On screen the ropes are moved only by the frame callback, so every part of the window drawn in the same
        // frame shows the same rope. Without a window (a snapshot) there are no frames, so place them here.
        if (getPeer() == nullptr)
            update(false);
        {
            // The rack's cables stay off the open controls window's plate, which shows its own leads. Under the
            // window its backdrop dims them; only a carried cable lifts them over it.
            const juce::Graphics::ScopedSaveState rackLayer(g);
            const auto panel = plate();
            if (!panel.isEmpty())
                g.excludeClipRegion(panel.toNearestInt());
            // Depth rings around the patched knobs, under the cables.
            for (const auto& cable : cables)
                if (cable.knob != nullptr)
                    paintRing(g, cable);
            for (const auto& cable : cables)
                paintCable(g, cable);
        }
        // While a cable is carried, every free input it can reach glows in its colour, the controls window's
        // included; the one under the hand most.
        if (carrying && !tuning && moved)
        {
            const juce::Colour colour(colours[carrying->source]);
            for (int t = 1; t < static_cast<int>(targetKnobs.size()); ++t)
                for (auto* knob : dropKnobs(targetKnobs[static_cast<size_t>(t)]))
                    if (reachable(*knob))
                    {
                        const auto under = hover && hover->first == t && hover->second == knobBounds(*knob);
                        paintInputSocket(g, socketOf(*knob), under ? 1.f : .45f, colour);
                    }
        }
        paintLooseNames(g);
        paintDockedNames(g);
        if (carrying && !tuning && moved)
            paintCarried(g);
        for (size_t s = 0; s < sourceCount; ++s)
            paintJackLamp(g, s);
    }

private:
    struct Key
    {
        size_t source, slot;
        bool operator==(const Key&) const = default;
    };

    struct Rope
    {
        static constexpr int points = 20;
        std::array<juce::Point<float>, points> now {}, before {};
        bool ready = false, asleep = false;
        std::optional<juce::Point<float>> dropFrom;
        void reset() { ready = false; asleep = false; }
    };

    struct Cable
    {
        Key key;
        int target = 0;
        float amount = 0;
        juce::Point<float> from, to;
        std::optional<juce::Point<float>> exit;
        juce::Slider* knob = nullptr;
        juce::Rectangle<float> knobBounds;
        bool loose = false, docked = false, offscreen = false, lit = true;
        int stack = 0, shared = 1; // This plug's place among the plugs sharing its key, and how many share it.
    };

    static constexpr std::array<juce::uint32, sourceCount> colours { 0xffe0473c, 0xfff0b030, 0xff3c98e6, 0xff4fd18b,
                                                                      0xffb07cf0, 0xffe6c15a, 0xff8ff5c4 };
    static constexpr std::array<const char*, sourceCount> prefixes { "lfo_1_", "lfo_2_", "macro_", "env_", "seq_",
                                                                      "xy_x_", "xy_y_" };

    static constexpr const auto& targetKnobs = modulationKnobs;

    static size_t index(Key key) { return key.source * routesPerSource + key.slot; }

    juce::RangedAudioParameter* parameter(Key key, const char* field) const
    {
        return state.getParameter(juce::String(prefixes[key.source]) + field + "_" + juce::String(static_cast<int>(key.slot + 1)));
    }

    int target(Key key) const
    {
        const auto* p = parameter(key, "target");
        return p != nullptr ? juce::roundToInt(p->convertFrom0to1(p->getValue())) : 0;
    }

    float depth(Key key) const
    {
        const auto* p = parameter(key, "depth");
        return p != nullptr ? p->convertFrom0to1(p->getValue()) : 0.f;
    }

    static void write(juce::RangedAudioParameter* p, float value)
    {
        if (p == nullptr)
            return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(value));
        p->endChangeGesture();
    }

    void setDepth(Key key, float amount)
    {
        write(parameter(key, "depth"), juce::jlimit(-100.f, 100.f, std::round(amount)));
        wake();
    }

    void connect(Key key, int knobTarget, float amount)
    {
        write(parameter(key, "target"), static_cast<float>(knobTarget));
        write(parameter(key, "depth"), amount);
        if (isMacro(key.source))
            write(parameter(key, "active"), 1.f);
        wake();
    }

    void disconnect(Key key)
    {
        write(parameter(key, "target"), 0.f);
        ropes[index(key)].reset();
        wake();
    }

    std::optional<size_t> freeSlot(size_t source) const
    {
        for (size_t slot = 0; slot < routesPerSource; ++slot)
            if (target({ source, slot }) == 0)
                return slot;
        return std::nullopt;
    }

    std::optional<size_t> jackAt(juce::Point<float> at) const
    {
        // The whole tile counts, not just the socket, so the jack is easy to hit.
        for (size_t s = 0; s < sourceCount; ++s)
            if ((host.jackTile && host.jackTile(s).contains(at)) || (host.jack && host.jack(s).getDistanceFrom(at) < 11.f))
                return s;
        return std::nullopt;
    }

    std::optional<Key> plugAt(juce::Point<float> at) const
    {
        for (const auto& cable : cables)
            if (cable.to.getDistanceFrom(at) < 10.f && !cable.offscreen)
                return cable.key;
        return std::nullopt;
    }

    // The knob under a point that a cable can drive, as its modulation target and knob bounds.
    std::optional<std::pair<int, juce::Rectangle<float>>> knobAt(juce::Point<float> at) const
    {
        for (int t = 1; t < static_cast<int>(targetKnobs.size()); ++t)
            for (auto* knob : dropKnobs(targetKnobs[static_cast<size_t>(t)]))
            {
                const auto bounds = knobBounds(*knob);
                const auto near = bounds.expanded(10.f).contains(at) || socketOf(*knob).getDistanceFrom(at) < 12.f;
                if (near && reachable(*knob))
                    return std::pair { t, bounds };
            }
        return std::nullopt;
    }

    // Which of an effect's copies a route's cable goes to: the one it was dropped on while that is in view, else
    // the first copy in view, else the first in the rack.
    juce::Slider* choose(Key key, const std::vector<juce::Slider*>& knobs) const
    {
        if (knobs.empty())
            return nullptr;
        const auto window = host.rackWindow();
        const auto inView = [this, &window](juce::Slider* knob) { return window.contains(socketOf(*knob)); };
        if (auto* dropped = droppedOn[index(key)].getComponent())
            if (std::find(knobs.begin(), knobs.end(), dropped) != knobs.end() && inView(dropped))
                return dropped;
        for (auto* knob : knobs)
            if (inView(knob))
                return knob;
        return knobs.front();
    }

    juce::Rectangle<float> knobBounds(juce::Slider& knob) const
    {
        const auto layout = knob.getLookAndFeel().getSliderLayout(knob);
        return getLocalArea(&knob, knobArea(knob, layout.sliderBounds));
    }

    // The knob's input socket, where a plug goes, in this layer's coordinates.
    juce::Point<float> socketOf(juce::Slider& knob) const
    {
        const auto layout = knob.getLookAndFeel().getSliderLayout(knob);
        const auto local = knobArea(knob, layout.sliderBounds);
        return getLocalPoint(&knob, inputSocket(knob, local));
    }

    // Reads the routes and places every cable's ends; returns true when anything moved.
    bool update(bool simulate)
    {
        auto changed = false;
        std::vector<Cable> next;
        for (size_t s = 0; s < sourceCount; ++s)
        {
            const auto* active = state.getRawParameterValue(juce::String(prefixes[s]) + "active");
            for (size_t slot = 0; slot < routesPerSource; ++slot)
            {
                const Key key { s, slot };
                if (carrying && *carrying == key && !tuning && moved)
                    continue;
                Cable cable;
                cable.key = key;
                cable.target = target(key);
                if (cable.target <= 0 || cable.target >= static_cast<int>(targetKnobs.size()))
                    continue;
                cable.amount = depth(key);
                cable.lit = (active == nullptr || active->load(std::memory_order_relaxed) >= .5f)
                         && (!isMacro(s) || parameter(key, "active") == nullptr || parameter(key, "active")->getValue() >= .5f);
                cable.from = host.jack(s);
                cable.knob = choose(key, host.findKnobs(targetKnobs[static_cast<size_t>(cable.target)]));
                if (cable.knob != nullptr)
                {
                    cable.knobBounds = knobBounds(*cable.knob);
                    // The plug sits in the knob's input socket; seen from above, the cable leaves from under its round
                    // boot in whatever direction it hangs.
                    cable.to = socketOf(*cable.knob);
                    const auto window = host.rackWindow();
                    if (!window.contains(cable.to))
                    {
                        cable.offscreen = true;
                        cable.to = { juce::jlimit(window.getX() + 4.f, window.getRight() - 4.f, cable.to.x), cable.to.y };
                    }
                }
                else if (const auto key = host.findControlsKey ? host.findControlsKey(targetKnobs[static_cast<size_t>(cable.target)])
                                                               : std::nullopt)
                {
                    // The knob is on the card but only in its full controls: the plug goes into the card's key to
                    // that panel.
                    cable.docked = true;
                    cable.to = *key;
                    cable.offscreen = !host.rackWindow().contains(cable.to);
                    cable.stack = static_cast<int>(std::count_if(next.begin(), next.end(), [&cable](const Cable& other)
                        { return other.docked && other.to.getDistanceFrom(cable.to) < 1.f; }));
                }
                else
                {
                    // The cartridge isn't in the rack: the plug rests just left of its own jack, below the jack's
                    // name and clear of its neighbours; hovering the jack names where it would go.
                    cable.loose = true;
                    cable.to = cable.from + juce::Point<float>(-13.f, -1.f - 5.f * static_cast<float>(slot));
                }
                next.push_back(cable);
            }
        }
        for (auto& cable : next)
            if (cable.docked)
                cable.shared = static_cast<int>(std::count_if(next.begin(), next.end(), [&cable](const Cable& other)
                    { return other.docked && other.to.getDistanceFrom(cable.to) < 1.f; }));
        const auto floor = static_cast<float>(getHeight()) - 5.f;
        for (auto& cable : next)
            changed = step(ropes[index(cable.key)], cable.from, cable.to, cable.offscreen ? std::nullopt : cable.exit,
                           floor, simulate) || changed;
        changed = changed || next.size() != cables.size();
        cables = std::move(next);
        return changed;
    }

    // One frame of the rope: Verlet points under gravity, held at both ends, given some slack and resting on the
    // deck's floor. Once it has settled it sleeps, untouched, until an end moves; so a rope never creeps without
    // being repainted. Returns true when the rope moved this frame.
    static bool step(Rope& rope, juce::Point<float> from, juce::Point<float> to, std::optional<juce::Point<float>> exit,
                     float floor, bool simulate)
    {
        constexpr auto count = Rope::points;
        // Held points: the jack end, the plug end and, when the plug points somewhere, the plug's stiff tail.
        const auto pinned = [&exit](int i) { return i == 0 || i == count - 1 || (exit.has_value() && i == count - 2); };
        // Without frames to swing it (a snapshot), a rope whose ends have moved is simply hung again.
        if (!simulate && rope.ready
            && (rope.now.front().getDistanceFrom(from) > .5f || rope.now.back().getDistanceFrom(to) > .5f))
            rope.ready = false;
        const auto straight = from.getDistanceFrom(to);
        // Only a little longer than the straight run: a gentle sag, never a loop lying on the deck.
        const auto slack = straight * 1.025f + 6.f;
        const auto segment = slack / static_cast<float>(count - 1);
        if (!rope.ready)
        {
            // Start on a sagging curve so a still picture (and the first frame) looks right.
            const auto start = rope.dropFrom.value_or(to);
            for (int i = 0; i < count; ++i)
            {
                const auto t = static_cast<float>(i) / static_cast<float>(count - 1);
                const auto end = from + (start - from) * t;
                const auto sag = end.translated(0, std::sin(t * juce::MathConstants<float>::pi) * slack * .06f);
                rope.now[static_cast<size_t>(i)] = { sag.x, juce::jmin(sag.y, floor) };
            }
            if (exit)
                rope.now[static_cast<size_t>(count - 2)] = *exit;
            rope.before = rope.now;
            rope.ready = true;
            rope.dropFrom.reset();
            if (!simulate)
            {
                rope.now.front() = from;
                rope.now.back() = to;
                return true;
            }
        }
        if (!simulate)
            return false;
        if (rope.asleep)
        {
            if (rope.now.front().getDistanceFrom(from) < .01f && rope.now.back().getDistanceFrom(to) < .01f)
                return false;
            rope.asleep = false;
        }

        auto motion = 0.f;
        constexpr float gravity = 1400.f, dt = 1.f / 60.f, damping = .965f;
        for (int i = 1; i < count - 1; ++i)
        {
            if (pinned(i))
                continue;
            auto& p = rope.now[static_cast<size_t>(i)];
            auto& q = rope.before[static_cast<size_t>(i)];
            const auto velocity = (p - q) * damping;
            q = p;
            p += velocity + juce::Point<float>(0, gravity * dt * dt);
        }
        const auto oldFirst = rope.now.front(), oldLast = rope.now.back();
        rope.now.front() = rope.before.front() = from;
        rope.now.back() = rope.before.back() = to;
        if (exit)
            rope.now[static_cast<size_t>(count - 2)] = rope.before[static_cast<size_t>(count - 2)] = *exit;
        motion = juce::jmax(oldFirst.getDistanceFrom(from), oldLast.getDistanceFrom(to));
        for (int pass = 0; pass < 14; ++pass)
        {
            for (int i = 0; i < count - 1; ++i)
            {
                auto& a = rope.now[static_cast<size_t>(i)];
                auto& b = rope.now[static_cast<size_t>(i + 1)];
                const auto delta = b - a;
                const auto length = juce::jmax(.001f, delta.getDistanceFromOrigin());
                if (length <= segment)
                    continue;
                const auto fix = delta * ((length - segment) / length);
                const auto wa = pinned(i) ? 0.f : 1.f, wb = pinned(i + 1) ? 0.f : 1.f;
                if (wa + wb <= 0.f)
                    continue;
                a += fix * (wa / (wa + wb));
                b -= fix * (wb / (wa + wb));
            }
            for (int i = 1; i < count - 1; ++i)
            {
                if (pinned(i))
                    continue;
                auto& p = rope.now[static_cast<size_t>(i)];
                if (p.y > floor)
                {
                    // Friction where the cable lies on the deck.
                    p.y = floor;
                    p.x += (rope.before[static_cast<size_t>(i)].x - p.x) * .3f;
                }
            }
        }
        for (int i = 1; i < count - 1; ++i)
            motion = juce::jmax(motion, rope.now[static_cast<size_t>(i)].getDistanceFrom(rope.before[static_cast<size_t>(i)]));
        if (motion < .02f)
        {
            // Settled: stop exactly here, so what is on screen is where the rope stays.
            rope.before = rope.now;
            rope.asleep = true;
        }
        return true;
    }

    juce::Path ropePath(const Rope& rope) const
    {
        juce::Path path;
        path.startNewSubPath(rope.now.front());
        for (size_t i = 1; i + 1 < rope.now.size(); ++i)
        {
            const auto middle = (rope.now[i] + rope.now[i + 1]) * .5f;
            path.quadraticTo(rope.now[i], middle);
        }
        path.lineTo(rope.now.back());
        return path;
    }

    void strokeCable(juce::Graphics& g, const juce::Path& path, juce::Colour colour, float alpha) const
    {
        g.setColour(juce::Colours::black.withAlpha(.32f * alpha));
        g.strokePath(path, juce::PathStrokeType(5.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::translation(3.f, 6.f));
        g.setColour(colour.darker(.45f).withAlpha(alpha));
        g.strokePath(path, juce::PathStrokeType(4.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(colour.withAlpha(alpha));
        g.strokePath(path, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(juce::Colours::white.withAlpha(.3f * alpha));
        g.strokePath(path, juce::PathStrokeType(1.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::translation(-.6f, -.9f));
    }

    // A jack plug: a coloured boot and a nickel sleeve pointing along the cable's last stretch.
    // A jack plug pushed into a panel socket, seen from above like the rest of the deck: a round coloured boot with
    // a turned metal cap, its shadow falling down and to the right. `towards` is unused for the round plug but kept
    // for where the cable comes from.
    static void paintPlug(juce::Graphics& g, juce::Point<float> tip, juce::Point<float>, juce::Colour colour, float alpha)
    {
        const auto boot = juce::Rectangle<float>(13.f, 13.f).withCentre(tip);
        g.setColour(juce::Colours::black.withAlpha(.4f * alpha));
        g.fillEllipse(boot.translated(1.5f, 2.5f).expanded(.5f));
        g.setGradientFill(juce::ColourGradient(colour.brighter(.35f).withAlpha(alpha), boot.getX(), boot.getY(),
                                               colour.darker(.55f).withAlpha(alpha), boot.getRight(), boot.getBottom(), false));
        g.fillEllipse(boot);
        g.setColour(juce::Colours::black.withAlpha(.55f * alpha));
        g.drawEllipse(boot.reduced(.3f), .8f);
        // Knurled grip round the boot.
        g.setColour(juce::Colours::black.withAlpha(.22f * alpha));
        for (int k = 0; k < 12; ++k)
        {
            const auto a = static_cast<float>(k) * juce::MathConstants<float>::twoPi / 12.f;
            const juce::Point<float> direction(std::cos(a), std::sin(a));
            g.drawLine(juce::Line<float>(tip + direction * 4.6f, tip + direction * 6.2f), .7f);
        }
        const auto cap = boot.reduced(3.6f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff2f4f6).withAlpha(alpha), cap.getX(), cap.getY(),
                                               juce::Colour(0xff6e7378).withAlpha(alpha), cap.getRight(), cap.getBottom(), false));
        g.fillEllipse(cap);
        g.setColour(juce::Colours::white.withAlpha(.5f * alpha));
        g.fillEllipse(juce::Rectangle<float>(2.f, 2.f).withCentre(cap.getCentre().translated(-1.f, -1.f)));
    }

    void paintCable(juce::Graphics& g, const Cable& cable) const
    {
        const auto& rope = ropes[index(cable.key)];
        if (!rope.ready)
            return;
        const auto colour = juce::Colour(colours[cable.key.source]);
        const auto alpha = (cable.lit ? 1.f : .45f) * (cable.offscreen ? .5f : 1.f);
        const auto path = ropePath(rope);
        // Cables lie on top of everything on the cards, readouts included.
        strokeCable(g, path, colour, alpha);
        paintPlug(g, rope.now.back(), rope.now[rope.now.size() - 2], colour, alpha);
        // Where the cable leaves the jack, its boot covers the socket.
        g.setColour(colour.darker(.35f).withAlpha(alpha));
        g.fillEllipse(juce::Rectangle<float>(11.f, 11.f).withCentre(cable.from));
        g.setColour(juce::Colours::white.withAlpha(.25f * alpha));
        g.drawEllipse(juce::Rectangle<float>(11.f, 11.f).withCentre(cable.from), .8f);
        // Plugs in a card's input carry no label; hovering the input lists the knobs they reach.
    }

    // Hovering a card's controls key with plugs in it lists the knobs they reach, below the key.
    void paintDockedNames(juce::Graphics& g) const
    {
        if (!hoveredDock)
            return;
        auto row = 0;
        for (const auto& cable : cables)
            if (cable.docked && !cable.offscreen && cable.to.getDistanceFrom(*hoveredDock) < 1.f)
                paintTag(g, juce::Rectangle<float>(80.f, 12.f).withPosition(cable.to.x + 8.f, cable.to.y + 4.f + 14.f * static_cast<float>(row++)),
                         knobName(cable.target), juce::Colour(colours[cable.key.source]), 1.f);
    }

    // The knob a route reaches, without the effect's name: VOICES for grain_voices.
    static juce::String knobName(int target)
    {
        const auto id = juce::String(targetKnobs[static_cast<size_t>(target)]);
        return id.fromFirstOccurrenceOf("_", false, false).replaceCharacter('_', ' ').toUpperCase();
    }

    static void paintTag(juce::Graphics& g, juce::Rectangle<float> tag, const juce::String& text, juce::Colour colour, float alpha)
    {
        g.setFont(font(8.5f, true));
        const auto width = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 8.f;
        tag.setWidth(width);
        g.setColour(juce::Colour(0xff11161b).withAlpha(.85f * alpha));
        g.fillRoundedRectangle(tag, 3.f);
        g.setColour(colour.withAlpha(.6f * alpha));
        g.drawRoundedRectangle(tag.reduced(.5f), 3.f, .8f);
        g.setColour(cream.withAlpha(.9f * alpha));
        g.drawText(text, tag, juce::Justification::centred);
    }

    // Hovering a jack lists the knobs its loose plugs would reach, stacked above the jack's tile.
    void paintLooseNames(juce::Graphics& g) const
    {
        if (!hoveredJack)
            return;
        auto row = 0;
        const auto tile = host.jackTile ? host.jackTile(*hoveredJack) : juce::Rectangle<float>();
        for (const auto& cable : cables)
            if (cable.loose && cable.key.source == *hoveredJack)
            {
                const auto tag = juce::Rectangle<float>(80.f, 12.f).withPosition(tile.getX(), tile.getY() - 30.f - 14.f * static_cast<float>(row++));
                paintTag(g, tag, juce::String(targetKnobs[static_cast<size_t>(cable.target)]).replaceCharacter('_', ' ').toUpperCase(),
                         juce::Colour(colours[cable.key.source]), 1.f);
            }
    }

    // The modulation range around a patched knob: an arc in the cable's colour from the knob's setting to where the
    // source can push it.
    void paintRing(juce::Graphics& g, const Cable& cable) const
    {
        if (cable.offscreen || cable.knob == nullptr)
            return;
        const auto rotary = cable.knob->getRotaryParameters();
        const auto proportion = static_cast<float>(cable.knob->valueToProportionOfLength(cable.knob->getValue()));
        const auto start = rotary.startAngleRadians, end = rotary.endAngleRadians;
        const auto from = start + (end - start) * proportion;
        // An LFO or the sequencer swings both ways; the Macro and the envelope push one way from where the knob sits.
        const auto reach = (end - start) * cable.amount * .01f;
        const auto oneWay = cable.key.source == 3;
        const auto a0 = oneWay ? from : from - std::abs(reach);
        const auto a1 = oneWay ? from + reach : from + std::abs(reach);
        // Drawn on the knob itself, just inside its rim, so it never reaches the card's lettering or scales.
        const auto radius = cable.knobBounds.getWidth() * .47f - 1.8f;
        juce::Path arc;
        arc.addCentredArc(cable.knobBounds.getCentreX(), cable.knobBounds.getCentreY(), radius, radius, 0,
                          juce::jlimit(start, end, juce::jmin(a0, a1)), juce::jlimit(start, end, juce::jmax(a0, a1)), true);
        const auto colour = juce::Colour(colours[cable.key.source]).withAlpha(cable.lit ? .95f : .4f);
        // A dark edge under the coloured arc keeps it readable on light and bright card finishes alike.
        g.setColour(juce::Colours::black.withAlpha(cable.lit ? .7f : .35f));
        g.strokePath(arc, juce::PathStrokeType(4.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(colour);
        g.strokePath(arc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (plugAt(getMouseXYRelative().toFloat()) == cable.key)
        {
            // The depth on a small dark tag beside the plug, so it never prints over the card's own lettering.
            const auto text = (cable.amount > 0 ? "+" : "") + juce::String(juce::roundToInt(cable.amount)) + "%";
            const auto typeface = readoutFont(10.5f);
            const auto width = juce::GlyphArrangement::getStringWidth(typeface, text) + 10.f;
            const juce::Rectangle<float> tag(cable.to.x + 10.f, cable.to.y - 8.f, width, 16.f);
            g.setColour(juce::Colour(0xf00b0d10));
            g.fillRoundedRectangle(tag, 4);
            g.setColour(colour.withAlpha(.9f));
            g.drawRoundedRectangle(tag.reduced(.5f), 4, 1);
            g.setFont(typeface);
            g.setColour(cream);
            g.drawText(text, tag, juce::Justification::centred);
        }
    }

    // The input socket of the knob under the hand while a cable is carried.
    juce::Point<float> targetSocket() const
    {
        if (hover)
            for (auto* knob : dropKnobs(targetKnobs[static_cast<size_t>(hover->first)]))
                if (knobBounds(*knob) == hover->second)
                    return socketOf(*knob);
        return hand;
    }

    void paintCarried(juce::Graphics& g) const
    {
        const auto colour = juce::Colour(colours[carrying->source]);
        const auto from = host.jack(carrying->source);
        // With a card's controls open the carried cable is only hinted: a small plug at the hand on a fine dotted
        // thread, from the jack while the hand is over the rack and from the window's IN socket over the window.
        // Over a knob it can go into, the thread becomes a thin preview of the lead it will make.
        if (const auto inputs = host.panelInputs ? host.panelInputs() : std::nullopt)
        {
            const auto in = inputs->second;
            const auto onPlate = plate().contains(hand);
            const auto start = onPlate ? in : from;
            const auto target = hover ? std::optional<juce::Point<float>>(targetSocket()) : std::nullopt;
            const auto end = target ? *target : hand;
            juce::Path thread;
            thread.startNewSubPath(start);
            const auto sag = juce::jmax(16.f, std::abs(end.x - start.x) * .15f);
            if (onPlate)
                thread.cubicTo(start.translated(0, sag), end.translated(0, -sag), end);
            else
                thread.quadraticTo((start + end) * .5f + juce::Point<float>(0, sag), end);
            if (target)
            {
                g.setColour(colour.withAlpha(.85f));
                g.strokePath(thread, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            else
            {
                juce::Path dotted;
                const float pattern[] { 2.f, 4.f };
                juce::PathStrokeType(1.2f).createDashedStroke(dotted, thread, pattern, 2);
                g.setColour(colour.withAlpha(.6f));
                g.fillPath(dotted);
            }
            if (onPlate)
                paintPanelSocket(g, in, 14.f, 1.f, true);
            g.setColour(colour.withAlpha(.25f));
            g.fillEllipse(juce::Rectangle<float>(14.f, 14.f).withCentre(hand));
            g.setColour(colour.darker(.25f));
            g.fillEllipse(juce::Rectangle<float>(7.f, 7.f).withCentre(hand));
            g.setColour(juce::Colours::white.withAlpha(.5f));
            g.fillEllipse(juce::Rectangle<float>(2.f, 2.f).withCentre(hand.translated(-1.f, -1.f)));
            return;
        }
        const auto slack = from.getDistanceFrom(hand) * 1.1f + 20.f;
        juce::Path path;
        path.startNewSubPath(from);
        const auto middle = (from + hand) * .5f;
        path.quadraticTo(middle.translated(0, slack * .22f), hand);
        strokeCable(g, path, colour, 1.f);
        paintPlug(g, hand, middle.translated(0, slack * .22f), colour, 1.f);
    }

    void paintJackLamp(juce::Graphics& g, size_t source) const
    {
        if (!host.jack)
            return;
        const auto busy = std::any_of(cables.begin(), cables.end(), [source](const Cable& c) { return c.key.source == source; });
        const auto centre = host.jack(source).translated(12.f, -9.f);
        const auto colour = juce::Colour(colours[source]);
        if (busy)
        {
            g.setColour(colour.withAlpha(.3f));
            g.fillEllipse(juce::Rectangle<float>(8.f, 8.f).withCentre(centre));
        }
        g.setColour(busy ? colour : colour.withAlpha(.25f));
        g.fillEllipse(juce::Rectangle<float>(4.f, 4.f).withCentre(centre));
    }

    void showPlugMenu(Key key)
    {
        juce::PopupMenu menu, depths;
        for (const auto amount : { 100, 75, 50, 25, 10, -10, -25, -50, -75, -100 })
            depths.addItem(1000 + amount, (amount > 0 ? "+" : "") + juce::String(amount) + "%", true,
                           juce::roundToInt(depth(key)) == amount);
        menu.addSubMenu("Depth", depths);
        menu.addItem(1, "Invert");
        menu.addSeparator();
        menu.addItem(2, "Pull out");
        menu.setLookAndFeel(&getLookAndFeel());
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
            [safe = juce::Component::SafePointer<PatchCables>(this), key](int id)
            {
                if (safe == nullptr || id == 0)
                    return;
                if (id == 1)
                    safe->setDepth(key, -safe->depth(key));
                else if (id == 2)
                    safe->disconnect(key);
                else
                    safe->setDepth(key, static_cast<float>(id - 1000));
            });
    }

    // Keeps the ropes swinging until they settle. While anything moves the whole layer is repainted at once:
    // repainting only the cables' surroundings left pieces of older frames behind on some systems.
    void tick()
    {
        openRestingKey();
        const auto moving = update(true);
        if (moving || std::exchange(wasMoving, false) || carrying)
            repaint();
    }

    void wake()
    {
        wasMoving = true;
        repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    Host host;
    std::vector<Cable> cables;
    std::array<Rope, sourceCount * routesPerSource> ropes;
    std::optional<size_t> hoveredJack;
    std::optional<juce::Point<float>> hoveredDock;
    std::array<juce::Component::SafePointer<juce::Slider>, sourceCount * routesPerSource> droppedOn;
    std::optional<Key> carrying;
    std::optional<size_t> pressedJack;
    std::optional<std::pair<int, juce::Rectangle<float>>> hover;
    juce::Point<float> hand, pressedAt;
    bool carryingNew = false, tuning = false, moved = false, wasMoving = true, raised = false;
    float tuneStart = 0;
    int restingKey = -1, openedKey = -1;
    double restingSince = 0;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// Finds a component by its ID anywhere below `root`.
juce::Component* findById(juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (child->getComponentID() == id)
            return child;
        if (auto* found = findById(*child, id))
            return found;
    }
    return nullptr;
}

// The header's live badge: the three VHS stripes slowly glow up and fade one after another, never in a
// fixed order, while the MELTIT tube rolls, hums and now and then loses its sync like an old television set.
class HeaderBadge final : public juce::Component
{
public:
    HeaderBadge()
    {
        setComponentID("header_badge");
        setInterceptsMouseClicks(false, false);
    }

    void place(Area header)
    {
        headerArea = header;
        setBounds(retro::headerBadgeArea(header));
    }

    // One stripe's glow: the colour swells slowly, holds a moment, then dies away just as gently.
    static float flash(double t)
    {
        constexpr double rise = 1.2, hold = .4, fall = 1.6;
        if (t <= 0 || t >= rise + hold + fall)
            return 0;
        if (t < rise)
            return (float) (.5 - .5 * std::cos(juce::MathConstants<double>::pi * t / rise));
        if (t < rise + hold)
            return 1;
        return (float) (.5 + .5 * std::cos(juce::MathConstants<double>::pi * (t - rise - hold) / fall));
    }

    void paint(juce::Graphics& g) override
    {
        g.addTransform(juce::AffineTransform::translation((float) -getX(), (float) -getY()));
        retro::headerBadge(g, headerArea, frame);
    }

private:
    void tick()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        if (now - lastFrame < 1.0 / 30)
            return;
        lastFrame = now;
        if (started < 0)
        {
            started = now;
            nextFlash = now + .6;
            nextGlitch = now + 2.5 + random.nextDouble() * 3;
        }

        // Stripes: each flash picks a stripe other than the last; sometimes a second one answers right after.
        if (now >= nextFlash)
        {
            auto pick = random.nextInt(2);
            pick = (lastStripe + 1 + pick) % 3;
            ignite[(size_t) pick] = now;
            if (random.nextFloat() < .3f)
                ignite[(size_t) ((pick + 1 + random.nextInt(2)) % 3)] = now + .8 + random.nextDouble() * .5;
            lastStripe = pick;
            nextFlash = now + 1.6 + random.nextDouble() * 1.4;
        }
        for (size_t i = 0; i < 3; ++i)
            frame.stripes[i] = flash(now - ignite[i]);

        // Tube: a slow roll, a light flicker, and an occasional tear of the picture.
        frame.roll = (float) std::fmod((now - started) / 4.5, 1.0);
        frame.flicker = random.nextFloat() < .06f ? .12f + random.nextFloat() * .12f : random.nextFloat() * .045f;
        if (now >= nextGlitch)
        {
            glitchStart = now;
            glitchLength = .12 + random.nextDouble() * .22;
            nextGlitch = now + 3.5 + random.nextDouble() * 5.5;
        }
        const auto glitchAge = now - glitchStart;
        frame.glitch = glitchAge < glitchLength ? (float) (1 - glitchAge / glitchLength) * (.6f + .4f * random.nextFloat()) : 0.f;
        frame.seed = random.nextInt();
        repaint();
    }

    Area headerArea;
    retro::BadgeState frame;
    juce::Random random;
    std::array<double, 3> ignite { -10, -10, -10 };
    int lastStripe = 2;
    double started = -1, lastFrame = 0, nextFlash = 0, nextGlitch = 0, glitchStart = -10, glitchLength = 0;
    juce::VBlankAttachment frames { this, [this] { tick(); } };
};

// The row of cartridges lying on the shelf. It is wider than the shelf once the cards are a readable size, so it
// sits in a viewport and scrolls sideways; each card casts its own shadow so the shadows travel with it.
class ShelfStrip final : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        const juce::DropShadow cardShadow(juce::Colours::black.withAlpha(.9f), 12, { 3, 6 });
        for (auto* child : getChildren())
            if (child->isVisible())
                cardShadow.drawForRectangle(g, child->getBounds().reduced(4, 3));
    }
};

class CartridgeSurface final : public juce::Component, private juce::Timer
{
public:
    CartridgeSurface(juce::AudioProcessor& processor, juce::AudioProcessorValueTreeState& parameters, EditorModel& editor)
        : processor(processor), state(parameters), model(editor), look(art)
    {
        setLookAndFeel(&look);
        setOpaque(true);
        styleButton(previous, ButtonFace::previous);
        styleButton(next, ButtonFace::next);
        styleButton(preset, ButtonFace::presetName);
        styleButton(generate, ButtonFace::generate);
        styleButton(options, ButtonFace::options);
        previous.setComponentID("previous_preset");
        previous.setTitle("Previous preset");
        next.setComponentID("next_preset");
        next.setTitle("Next preset");
        preset.setComponentID("presets");
        generate.setComponentID("generate");
        options.setComponentID("options");
        addAndMakeVisible(badge);
        for (auto* button : { &previous, &next, &preset, &generate, &options })
            addAndMakeVisible(*button);
        previous.onClick = [this] { stepPreset(-1); };
        next.onClick = [this] { stepPreset(1); };
        preset.onClick = [this] { showPresets(); };
        generate.onClick = [this] { generateRack(static_cast<uint32_t>(juce::Random::getSystemRandom().nextInt())); };
        options.onClick = [this] { showOptions(); };

        addAndMakeVisible(chain);
        chain.onSelect = [this](int position) { showSlot(position); };

        viewport.setComponentID("rack_viewport");
        viewport.setViewedComponent(&rack, false);
        viewport.setScrollBarsShown(false, true);
        viewport.setScrollBarThickness(8);
        viewport.setScrollOnDragEnabled(false);
        addAndMakeVisible(viewport);
        addAndMakeVisible(edgeShade);

        added.setComponentID("add_slot");
        added.setButtonText("+");
        added.setTitle("Click to add an empty slot or drop a cartridge here");
        added.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        added.artwork = [this](auto& g, auto area, bool hovered) { drawAdd(g, area, hovered); };
        added.onClick = [this] { insertAt(emptyCartridge, static_cast<int>(slots.size())); };
        addAndMakeVisible(added);

        for (size_t i = 0; i < modules.size(); ++i)
        {
            auto& card = shelf[i];
            card.setButtonText(modules[i].name);
            card.setTitle(juce::String("Drag ") + modules[i].name + " into the rack");
            card.setComponentID(juce::String(modules[i].prefix) + "_cartridge");
            card.setMouseCursor(juce::MouseCursor::PointingHandCursor);
            card.artwork = [this, i](auto& g, auto area, bool over)
                { art.drawCartridge(g, area, static_cast<int>(i), over); };
            card.onDrag = [this, i](const auto& e) { drag(e, static_cast<int>(i), -1, false); };
            card.onDrop = [this, i](const auto& e) { drag(e, static_cast<int>(i), -1, true); };
            card.onClick = [this, i]
            {
                const auto empty = std::find(order.begin(), order.end(), emptyCartridge);
                const auto position = empty == order.end() ? static_cast<int>(slots.size())
                                                           : static_cast<int>(empty - order.begin());
                insertAt(static_cast<int>(i), position);
            };
            shelfStrip.addAndMakeVisible(card);
        }
        shelfView.setComponentID("shelf_viewport");
        shelfView.setViewedComponent(&shelfStrip, false);
        shelfView.setScrollBarsShown(false, true);
        shelfView.setScrollBarThickness(shelfScrollBar);
        shelfView.setScrollOnDragEnabled(false);
        addAndMakeVisible(shelfView);
        addAndMakeVisible(master);
        addAndMakeVisible(modWindow);
        // The drawer's knobs were built before this surface had its look, so their value boxes were made by the
        // default one; telling them about the look now rebuilds those boxes as the skinned readouts.
        modWindow.sendLookAndFeelChange();
        modWindow.drawer.onChoose = [this](int source) { master.setOpenSource(source); };
        modWindow.drawer.onHide = [this] { toggleDrawer(-1); };
        modWindow.onShown = [this](float shown) { setDrawerSpace(juce::roundToInt(static_cast<float>(drawerSpace) * shown)); };
        PatchCables::Host host;
        host.findKnobs = [this](const juce::String& id)
        {
            // A route drives both copies of an effect, so either copy's knob takes the cable.
            std::vector<juce::Slider*> knobs;
            for (const auto& slot : slots)
                if (slot->isVisible() && !slot->isLeaving() && slot->drawer != nullptr)
                    for (const auto& name : { id, "cart2_" + id })
                        if (auto* knob = dynamic_cast<juce::Slider*>(findById(*slot->drawer, name)))
                            knobs.push_back(knob);
            return knobs;
        };
        host.findControlsKey = [this](const juce::String& id) -> std::optional<juce::Point<float>>
        {
            // Effect names are single words, so the knob's id starts with its card's prefix: grain_voices on Grain.
            const auto effect = id.upToFirstOccurrenceOf("_", true, false);
            for (const auto& slot : slots)
                if (slot->isVisible() && !slot->isLeaving() && slot->drawer != nullptr)
                {
                    const auto& key = slot->drawer->details;
                    const auto& keyId = key.getComponentID();
                    if (keyId == effect + "controls" || keyId == "cart2_" + effect + "controls")
                        return getLocalArea(&key, key.getLocalBounds()).toFloat().getCentre();
                }
            return std::nullopt;
        };
        host.findPanelKnob = [this](const juce::String& id) -> juce::Slider*
        {
            return inspector != nullptr ? inspector->knobFor(id) : nullptr;
        };
        host.panelPlate = [this]
        {
            return inspector != nullptr ? inspector->restingPlate() + inspector->getPosition().toFloat() : juce::Rectangle<float>();
        };
        host.raise = [this](bool carrying)
        {
            if (carrying && cables != nullptr)
                cables->toFront(false);
            else if (inspector != nullptr)
                inspector->toFront(false);
        };
        host.controlsKeyAt = [this](juce::Point<float> at)
        {
            for (const auto& slot : slots)
                if (slot->isVisible() && !slot->isLeaving() && slot->drawer != nullptr)
                {
                    const auto& key = slot->drawer->details;
                    if (getLocalArea(&key, key.getLocalBounds()).toFloat().expanded(8.f).contains(at))
                        return slot->instance;
                }
            return -1;
        };
        host.openControls = [this](int instance)
        {
            inspectCartridge(instance);
            // The cable is still in the hand: keep it over the window that just opened.
            if (cables != nullptr)
                cables->toFront(false);
        };
        host.panelInputs = [this]() -> std::optional<std::pair<juce::Point<float>, juce::Point<float>>>
        {
            if (inspector == nullptr || inspectedInstance < 0 || inspector->restingPlate().isEmpty())
                return std::nullopt;
            for (const auto& slot : slots)
                if (slot->instance == inspectedInstance && slot->drawer != nullptr)
                {
                    const auto& key = slot->drawer->details;
                    return std::pair { getLocalArea(&key, key.getLocalBounds()).toFloat().getCentre(),
                                       inspector->inputPoint() + inspector->getPosition().toFloat() };
                }
            return std::nullopt;
        };
        host.rackWindow = [this] { return viewport.getBounds().toFloat(); };
        host.jack = [this](size_t source) { return master.jack(source).getCentre() + master.getPosition().toFloat(); };
        host.jackTile = [this](size_t source) { return master.jackTile(source) + master.getPosition().toFloat(); };
        host.openSource = [this](size_t source) { toggleDrawer(static_cast<int>(source)); };
        host.notify = [this](const juce::String& heading, const juce::String& text) { notice.show(heading, text); };
        cables = std::make_unique<PatchCables>(state, std::move(host));
        addAndMakeVisible(*cables);
        addChildComponent(ghost);
        addChildComponent(notice);
        ghost.setInterceptsMouseClicks(false, false);
        order.fill(noCartridge);
        setSize(editorDesignWidth, editorDesignHeight);
        refresh();
        startTimerHz(30);
    }

    ~CartridgeSurface() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    // The deck itself, without the space the MOD drawer opens below it.
    Area deckBounds() const { return getLocalBounds().withHeight(editorDesignHeight); }

    void paint(juce::Graphics& g) override
    {
        // Behind the drawer: the dark cavity it slides out of.
        if (getHeight() > editorDesignHeight)
        {
            g.setColour(juce::Colour(0xff0a0c0e));
            g.fillRect(getLocalBounds().withTrimmedTop(editorDesignHeight));
        }
        background.draw(g, deckBounds(), [this](juce::Graphics& cg)
        {
            cg.fillAll(charcoal);
            art.drawDeck(cg, deckBounds());
            art.drawHeader(cg, getLocalBounds().removeFromTop(headerHeight));
            art.drawRack(cg, { 3, rackTop - 3, getWidth() - 6, tray.getY() - rackTop + 3 });
            art.drawPreset(cg, presetHousing);
            art.drawShelf(cg, tray.withX(3).withWidth(getWidth() - 6));
            art.drawRack(cg, masterHousing);
        });
        art.drawSignal(g, { 0, headerHeight, getWidth(), flowHeight }, peaks);
    }

    // Studio lighting: a warm wash from above and darker corners pull the eye to the rack.
    void paintOverChildren(juce::Graphics& g) override
    {
        const auto bounds = deckBounds().toFloat();
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffffe2b0).withAlpha(.05f), 0, bounds.getY(),
                                               juce::Colours::transparentWhite, 0, bounds.getHeight() * .45f, false));
        g.fillRect(bounds);
        juce::ColourGradient vignette(juce::Colours::transparentBlack, bounds.getCentreX(), bounds.getHeight() * .45f,
                                      juce::Colours::black.withAlpha(.42f), 0, 0, true);
        vignette.addColour(.55, juce::Colours::transparentBlack);
        g.setGradientFill(vignette);
        g.fillRect(bounds);
    }

    void resized() override
    {
        // The deck's picture only changes with its width; the drawer opening below it leaves it as it is.
        if (std::exchange(laidOutWidth, getWidth()) != getWidth())
            background.clear();
        badge.place(getLocalBounds().removeFromTop(headerHeight));
        auto header = getLocalBounds().removeFromTop(headerHeight).reduced(19, 5);
        options.setBounds(header.removeFromRight(40));
        header.removeFromRight(28);
        generate.setBounds(header.removeFromRight(115));
        // The preset browser sits centred in the gap between the wordmark and Generate.
        const auto logoRight = juce::roundToInt(retro::headerLogoRight(getLocalBounds().removeFromTop(headerHeight)));
        header.setLeft(juce::jmin(logoRight, header.getRight()));
        auto browser = header.withSizeKeepingCentre(juce::jmin(272, header.getWidth()), header.getHeight());
        presetHousing = browser;
        previous.setBounds(browser.removeFromLeft(39).reduced(2));
        next.setBounds(browser.removeFromRight(39).reduced(2));
        preset.setBounds(browser.reduced(6, 2));

        // Along the bottom: the master section, with the cartridge shelf above it.
        // The deck keeps its own height; while the MOD drawer is out, the surface grows below it.
        masterHousing = getLocalBounds().withHeight(editorDesignHeight).reduced(3, 0).withTrimmedBottom(6).removeFromBottom(54);
        master.setBounds(masterHousing.reduced(10, 7));
        tray = getLocalBounds().reduced(14, 0).withBottom(masterHousing.getY() - 5).removeFromBottom(120);
        layoutShelf();
        // The chain plates sit on the signal line, clear of the arrow head.
        const auto line = retro::signalLine({ 0, headerHeight, getWidth(), flowHeight });
        chain.setBounds(line.withTrimmedRight(18).reduced(0, 6).toNearestInt());
        layoutRack();
        // The full controls cover the shelf too: more room for them, and no cartridges go in while they are open.
        if (inspector)
            inspector->setBounds(rackArea().withBottom(tray.getBottom()));
        notice.setBounds(Area(380, 78).withCentre({ getWidth() / 2, rackTop + 16 + 39 }));
        modWindow.setBounds(masterHousing.getX() + 6, masterHousing.getBottom(), masterHousing.getWidth() - 12, ModDrawer::height);
        if (cables)
            cables->setBounds(getLocalBounds());
    }

private:
    void timerCallback() override { refresh(); }

    // Cards on the shelf keep a readable size; when they don't all fit, the strip scrolls sideways under a
    // scroll bar along the shelf's lower edge. When they do fit, they sit centred on the shelf.
    static constexpr int shelfCardHeight = 80, shelfCardGap = 12, shelfScrollBar = 10, shelfPad = 10;

    void layoutShelf()
    {
        const auto view = tray.reduced(8, 6);
        shelfView.setBounds(view);
        const auto cardWidth = juce::roundToInt(static_cast<float>(shelfCardHeight) * cartridgeAspect);
        const auto count = static_cast<int>(shelf.size());
        const auto rowWidth = count * cardWidth + (count - 1) * shelfCardGap + 2 * shelfPad;
        const auto stripHeight = view.getHeight() - shelfScrollBar - 2;
        shelfStrip.setSize(juce::jmax(view.getWidth(), rowWidth), stripHeight);
        const auto left = (shelfStrip.getWidth() - rowWidth) / 2 + shelfPad;
        const auto top = (stripHeight - shelfCardHeight) / 2 - 2;
        for (int i = 0; i < count; ++i)
            shelf[static_cast<size_t>(i)].setBounds(left + i * (cardWidth + shelfCardGap), top, cardWidth, shelfCardHeight);
        shelfView.setViewPosition(juce::jmin(shelfView.getViewPositionX(), shelfStrip.getWidth() - view.getWidth()), 0);
    }

    Area shelfCard(size_t type) const
    {
        const auto& card = shelf[type];
        return getLocalArea(&card, card.getLocalBounds());
    }

    // Brings a cartridge into view: the rack glides until the slot is centred, then its dock lights up briefly.
    void showSlot(int position)
    {
        if (position < 0 || position >= static_cast<int>(slots.size()))
            return;
        auto& slot = *slots[static_cast<size_t>(position)];
        const auto widest = juce::jmax(0, rack.getWidth() - viewport.getWidth());
        scrollTo = juce::jlimit(0, widest, slot.getBounds().getCentreX() - viewport.getWidth() / 2);
        scrollFrom = viewport.getViewPositionX();
        if (getPeer() == nullptr || scrollFrom == scrollTo)
            viewport.setViewPosition(scrollTo, 0);
        else
        {
            scrollStart = juce::Time::getMillisecondCounterHiRes();
            scrolling = true;
        }
        slot.highlight(true);
        juce::Timer::callAfterDelay(650, [safe = juce::Component::SafePointer<CartridgeSlot>(&slot)]
        {
            if (safe != nullptr)
                safe->highlight(false);
        });
    }

    void tickScroll()
    {
        if (!scrolling)
            return;
        const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((juce::Time::getMillisecondCounterHiRes() - scrollStart) * .001) / .38f);
        const auto eased = 1.f - (1.f - u) * (1.f - u) * (1.f - u);
        viewport.setViewPosition(juce::roundToInt(static_cast<float>(scrollFrom) + static_cast<float>(scrollTo - scrollFrom) * eased), 0);
        scrolling = u < 1.f;
    }

    Area rackArea() const
    {
        return { 7, rackTop, getWidth() - 14, tray.getY() - rackTop - 4 };
    }

    void layoutRack()
    {
        constexpr int plusWidth = 68, topInset = 7;
        const auto area = rackArea();
        const auto length = static_cast<int>(slots.size());
        for (int i = 0; i < length; ++i)
            slots[i]->setBounds(rackInset + i * slotPitch, topInset, slotWidth, slotHeight + drawerHeight);

        const bool canAdd = length < cartridgeSlots;
        const auto inlineX = rackInset + length * slotPitch;
        const bool pinPlus = canAdd && length > 0 && inlineX + plusWidth + rackInset > area.getWidth();
        auto cardsArea = area;
        if (pinPlus)
            cardsArea.removeFromRight(plusWidth + slotGap + rackInset);

        const auto lastCardRight = slots.empty() ? 0 : slots.back()->getRight();
        const auto contentWidth = juce::jmax(cardsArea.getWidth(), lastCardRight + (pinPlus ? 0 : rackInset));
        viewport.setBounds(cardsArea);
        edgeShade.setBounds(cardsArea);
        rack.setSize(contentWidth, juce::jmax(cardsArea.getHeight(), slotHeight + drawerHeight + 10));

        Area destination;
        if (length == 0)
        {
            const auto width = juce::jmin(440, area.getWidth() - 60);
            destination = Area(width, 161).withCentre({ area.getCentreX(), area.getY() + 140 });
        }
        else if (canAdd)
        {
            const auto scroll = contentWidth > cardsArea.getWidth() ? viewport.getScrollBarThickness() : 0;
            const auto x = pinPlus ? area.getRight() - rackInset - plusWidth : area.getX() + inlineX;
            const auto height = juce::jmax(slotHeight, area.getHeight() - scroll - 2 * topInset);
            destination = Area(x, area.getY() + topInset, plusWidth, height);
        }

        // When slots come or go the plus section glides to its new place; any other relayout, such as resizing
        // the window, puts it there at once.
        const auto slotsChanged = std::exchange(laidOutSlots, length) != length;
        const auto wasShown = added.isVisible() && !added.getBounds().isEmpty();
        if (canAdd && wasShown && getPeer() != nullptr && (slotsChanged || addMoving) && added.getBounds() != destination)
        {
            if (slotsChanged)
            {
                addFrom = added.getBounds().toFloat();
                addStarted = juce::Time::getMillisecondCounterHiRes();
            }
            addTo = destination.toFloat();
            addMoving = true;
        }
        else
        {
            addMoving = false;
            if (canAdd)
                added.setBounds(destination);
        }
        added.setVisible(canAdd);
    }

    // Eases the plus section towards its place, overshooting a touch like a sliding carriage hitting its stop.
    void tickAdd()
    {
        if (!addMoving)
            return;
        const auto u = juce::jlimit(0.f, 1.f, static_cast<float>((juce::Time::getMillisecondCounterHiRes() - addStarted) * .001) / .42f);
        const auto e = easeOutBack(u);
        const auto mix = [e](float a, float b) { return a + (b - a) * e; };
        added.setBounds(juce::Rectangle<float>(mix(addFrom.getX(), addTo.getX()), mix(addFrom.getY(), addTo.getY()),
                                               mix(addFrom.getWidth(), addTo.getWidth()),
                                               mix(addFrom.getHeight(), addTo.getHeight())).toNearestInt());
        if (u >= 1.f)
        {
            addMoving = false;
            added.setBounds(addTo.toNearestInt());
        }
    }

    void drawAdd(juce::Graphics& g, Area area, bool hovered) const
    {
        const bool active = target == static_cast<int>(slots.size()) || hovered;
        if (!slots.empty())
        {
            art.drawAdd(g, area, active ? 1.f : .94f);
            return;
        }

        art.draw(g, CartridgeImages::slot, area);
        g.setColour(cream);
        g.setFont(font(21, true));
        g.drawText("Drag a cartridge here", area.translated(0, -8), juce::Justification::centred);
        art.drawDock(g, Area(128, 23).withCentre({ area.getCentreX(), area.getCentreY() + 30 }), false);
        if (active)
        {
            g.setColour(accent.withAlpha(.85f));
            g.drawRoundedRectangle(area.toFloat().reduced(2), 11, 2);
        }
    }

    // Generate clears the rack with a flourish: the cartridges in it play their way out, then the new variation's
    // docks fall in empty while its cartridges lift off the shelf, gather in the middle of the deck and are dealt
    // into their slots one after another. Without a window on screen the rack changes at once.
    void generateRack(uint32_t seed)
    {
        if (dealing)
            return;
        if (getPeer() == nullptr)
        {
            model.generate(tag, seed);
            refresh();
            return;
        }
        dealing = true;
        int leaving = 0;
        for (auto& slot : slots)
        {
            if (!slot->isVisible() || slot->isLeaving())
                continue;
            juce::Timer::callAfterDelay(45 * leaving++, [safe = juce::Component::SafePointer<CartridgeSlot>(slot.get())]
            {
                if (safe != nullptr)
                    safe->remove([] {});
            });
        }
        // A slot's exit takes half a second; the deal starts once the last one has gone.
        const auto wait = leaving == 0 ? 0 : 45 * (leaving - 1) + 560;
        juce::Timer::callAfterDelay(wait, [safe = juce::Component::SafePointer<CartridgeSurface>(this), seed]
        {
            if (safe != nullptr)
                safe->deal(seed);
        });
    }

    void deal(uint32_t seed)
    {
        model.generate(tag, seed);
        // The old slots have all played out, so rebuild even if the variation keeps the same chain.
        order.fill(noCartridge);
        dealPending = true;
        refresh();
        dealPending = false;
        viewport.setViewPosition(0, 0);

        std::vector<int> waiting;
        for (int i = 0; i < static_cast<int>(slots.size()); ++i)
            if (slots[static_cast<size_t>(i)]->isAwaitingCartridge())
                waiting.push_back(i);
        if (waiting.empty())
        {
            dealing = false;
            return;
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto count = static_cast<int>(waiting.size());
        const auto window = viewport.getBounds().toFloat();
        const auto centre = window.getCentre().translated(0, -18);
        const auto cavityWidth = static_cast<float>(cavityOf(waiting.front()).getWidth());
        const auto spread = juce::jmin(46.f, window.getWidth() * .5f / static_cast<float>(count));
        constexpr double launchGap = 75, dealGap = 120, hold = 180;
        const auto dealStart = now + launchGap * (count - 1) + FlyingCartridge::riseTime + hold;
        const auto shelfWindow = shelfView.getBounds().toFloat();
        flights.clear();
        landed = 0;
        for (int k = 0; k < count; ++k)
        {
            const auto index = waiting[static_cast<size_t>(k)];
            auto* slot = slots[static_cast<size_t>(index)].get();
            const auto type = static_cast<size_t>(slot->instance) % modules.size();
            // Cards scrolled off the shelf leave from its nearest edge.
            auto from = CartridgeImages::fitCartridge(shelfCard(type)).toFloat();
            from.setCentre(juce::jlimit(shelfWindow.getX() + from.getWidth() * .5f,
                                        shelfWindow.getRight() - from.getWidth() * .5f, from.getCentreX()),
                           from.getCentreY());
            const auto fan = static_cast<float>(k) - static_cast<float>(count - 1) * .5f;
            FlyingCartridge::Flight flight;
            flight.from = from;
            flight.gather = FlyingCartridge::Box(cavityWidth * .78f, cavityWidth * .78f / cartridgeAspect)
                                .withCentre(centre.translated(fan * spread, fan * fan * 3.f));
            flight.to = cavityOf(index).toFloat();
            flight.fanAngle = fan * .09f;
            flight.bob = static_cast<float>(k) * 1.3f;
            flight.launch = now + launchGap * k;
            flight.deal = dealStart + dealGap * k;
            flight.fade = !window.contains(flight.to);
            auto card = std::make_unique<FlyingCartridge>(art, static_cast<int>(type), flight,
                [this, safe = juce::Component::SafePointer<CartridgeSlot>(slot)]
                {
                    if (safe != nullptr)
                        safe->seat();
                    if (++landed < static_cast<int>(flights.size()))
                        return;
                    // The last card is down; the cards are let go once their frame has finished.
                    juce::MessageManager::callAsync([surface = juce::Component::SafePointer<CartridgeSurface>(this)]
                    {
                        if (surface == nullptr)
                            return;
                        surface->flights.clear();
                        surface->dealing = false;
                    });
                });
            addChildComponent(*card);
            flights.push_back(std::move(card));
        }
    }

    void insertAt(int type, int position)
    {
        if (model.insertCartridge(type, position))
        {
            animatePosition = position;
            refresh();
            if (position < static_cast<int>(slots.size()))
            {
                const auto x = slots[position]->getRight() + rackInset - viewport.getWidth();
                if (x > viewport.getViewPositionX())
                    viewport.setViewPosition(x, 0);
            }
        }
        else
        {
            notice.show("Rack is full", "Use up to two copies of each effect and up to twelve slots.");
        }
    }

    void drag(const juce::MouseEvent& event, int type, int source, bool release)
    {
        const auto pt = getLocalPoint(event.eventComponent, event.getPosition());
        const auto rackPoint = rack.getLocalPoint(this, pt);
        const auto count = static_cast<int>(slots.size());
        int destination = -1;
        if (source < 0 && added.isVisible() && added.getBounds().contains(pt))
            destination = count;
        else if (viewport.getBounds().contains(pt))
        {
            for (int i = 0; i < count; ++i)
            {
                if ((source >= 0 || slots[i]->instance == emptyCartridge)
                    && slots[i]->getBounds().withHeight(slotHeight).contains(rackPoint))
                    destination = i;
            }
        }

        target = release ? -1 : destination;
        for (int i = 0; i < count; ++i)
            slots[i]->highlight(i == target);
        added.repaint();
        dragging = !release;
        const auto mouse = event.getEventRelativeTo(this).position;
        if (!release)
        {
            if (!ghost.carrying())
            {
                const auto home = source >= 0 ? cavityOf(source)
                                              : CartridgeImages::fitCartridge(shelfCard(static_cast<size_t>(type)));
                ghost.pickUp(type, home.toFloat(), mouse);
            }
            // Only pull towards a slot the user can see. The slot the plus button would add often lies past the
            // right edge of the rack, and pulling towards it dragged the cartridge out of the window.
            auto snap = destination >= 0 ? predictedCavity(destination) : Area();
            if (!viewport.getBounds().contains(snap))
                snap = {};
            ghost.follow(mouse, snap.toFloat());
        }
        else if (destination >= 0)
        {
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CartridgeSurface>(this),
                                             source, type, destination]
            {
                if (safe == nullptr)
                    return;
                if (source >= 0)
                {
                    safe->model.moveCartridge(source, destination);
                    safe->refresh();
                }
                else
                    safe->insertAt(type, destination);
                // Let the carried cartridge settle into the slot it now occupies.
                const auto placed = destination < static_cast<int>(safe->slots.size())
                                    && safe->slots[static_cast<size_t>(destination)]->instance >= 0;
                safe->ghost.drop(placed ? safe->cavityOf(destination).toFloat() : DragGhost::Box());
            });
        }
        else
            ghost.drop({});
    }

    // Where a seated cartridge sits in slot `index`, in this component's coordinates.
    Area cavityOf(int index) const
    {
        const auto slot = getLocalArea(&rack, slots[static_cast<size_t>(index)]->getBounds().withHeight(slotHeight));
        return slot.reduced(cavityInset, 0).withTrimmedTop(cavityTop).withTrimmedBottom(cavityBottom);
    }

    // Where the cartridge will sit after dropping it on `destination`, which may be the slot the plus button adds.
    Area predictedCavity(int destination) const
    {
        if (destination < static_cast<int>(slots.size()))
            return cavityOf(destination);
        const auto slot = getLocalArea(&rack, Area(rackInset + destination * slotPitch, 7, slotWidth, slotHeight));
        return slot.reduced(cavityInset, 0).withTrimmedTop(cavityTop).withTrimmedBottom(cavityBottom);
    }

    void refresh()
    {
        auto level = model.takeOutputPeaks();
        for (size_t channel = 0; channel < peaks.size(); ++channel)
            level[channel] = level[channel] < .002f && peaks[channel] < .003f
                ? 0 : std::max(level[channel], .78f * peaks[channel]);
        if (peaks != level)
        {
            peaks = level;
            repaint(0, headerHeight, getWidth(), flowHeight);
        }
        const auto now = juce::Time::getMillisecondCounterHiRes() * .001;
        if (std::max(level[0], level[1]) > .003f)
            lastSound = now;
        const auto activity = model.modulationActivity();
        master.setActivity(activity);
        if (modWindow.isOpen())
            modWindow.drawer.update(activity);
        const auto current = model.cartridges();
        if (!dragging && order != current)
        {
            order = current;
            inspector.reset();
            // Remember where each cartridge stood, so the ones that stay can glide to their new places.
            std::vector<std::pair<int, int>> previous;
            for (const auto& slot : slots)
                if (slot->isVisible())
                    previous.emplace_back(slot->instance, slot->getX());
            slots.clear();
            for (const auto instance : order)
            {
                if (instance == noCartridge)
                    break;
                const auto index = static_cast<int>(slots.size());
                auto slot = std::make_unique<CartridgeSlot>(art, state, instance, index, index == animatePosition);
                slot->eject.onClick = [this, index, instance, safe = juce::Component::SafePointer<CartridgeSlot>(slot.get())]
                {
                    if (safe == nullptr || safe->isLeaving())
                        return;
                    // The slot plays its way out first, its sound fading with it; eject only if the rack hasn't changed
                    // meanwhile, and otherwise bring the sound back.
                    if (instance >= 0)
                        model.fadeOutCartridge(instance, true);
                    // Captures the model itself, not this surface: the completion may run from the slot's destructor.
                    // Cartridges are found by instance, since an earlier eject may have shifted their positions.
                    safe->remove([&rackModel = model, index, instance]
                    {
                        const auto current = rackModel.cartridges();
                        auto position = static_cast<int>(std::find(current.begin(), current.end(), instance) - current.begin());
                        if (instance < 0)
                            position = current[static_cast<size_t>(index)] == instance ? index : cartridgeSlots;
                        if (position < cartridgeSlots)
                            rackModel.ejectCartridge(position);
                        else if (instance >= 0)
                            rackModel.fadeOutCartridge(instance, false);
                    });
                };
                if (instance >= 0)
                {
                    const auto type = instance % modules.size();
                    slot->insert.onDrag = [this, type, index](const auto& event)
                        { drag(event, static_cast<int>(type), index, false); };
                    slot->insert.onDrop = [this, type, index](const auto& event)
                        { drag(event, static_cast<int>(type), index, true); };
                    slot->insert.onClick = [this, instance] { inspectCartridge(instance); };
                    slot->drawer->details.onClick = [this, instance] { inspectCartridge(instance); };
                    slot->drawer->power.onClick = [this, index] { model.toggleCartridge(index); };
                    slot->drawer->onScrub = [this](bool engaged, float speed) { model.scrubTape(engaged, speed); };
                    if (juce::String(modules[type].prefix) == "stretch")
                        slot->drawer->stretchSource = [this, instance] { return model.stretchPreview(instance); };
                    if (juce::String(modules[type].prefix) == "grain")
                        slot->drawer->setGrainSource([this, instance] { return model.grainPicture(instance); });
                }
                // A cartridge landing in its dock knocks the rack: the cards beside it jump and ring out, the
                // nearest first and hardest.
                slot->onLanded = [this, index](float strength)
                {
                    for (int other = 0; other < static_cast<int>(slots.size()); ++other)
                    {
                        const auto distance = std::abs(other - index);
                        if (distance == 0)
                            continue;
                        slots[static_cast<size_t>(other)]->jolt(strength / (1.f + .9f * static_cast<float>(distance - 1)),
                                                                .025 * distance);
                    }
                };
                rack.addAndMakeVisible(*slot);
                slot->sendLookAndFeelChange();
                if (dealPending)
                {
                    if (instance >= 0)
                        slot->awaitCartridge();
                    else
                        slot->fallIn();
                }
                slots.push_back(std::move(slot));
            }
            added.setTitle(slots.empty() ? "Drag a cartridge here"
                                         : "Add an empty slot or drop a cartridge here");
            animatePosition = -1;
            layoutRack();
            // The slots' shadows reach past their bounds, so removing a slot alone would leave them behind on the
            // rack floor (switching to Initialize showed faint outlines of the previous cards).
            rack.repaint();
            edgeShade.repaint();
            // Slots that were already in the rack slide from where they stood; new ones play their own entrance.
            for (auto& slot : slots)
            {
                const auto match = std::find_if(previous.begin(), previous.end(),
                                                [&slot](const auto& entry) { return entry.first == slot->instance; });
                if (match == previous.end())
                    continue;
                slot->slideFrom(static_cast<float>(match->second - slot->getX()));
                previous.erase(match);
            }
        }
        // The tape runs while there is sound, at the speed the transport has it (stopped, scrubbed, rewound), and as
        // slowly as the most stretched Stretch in the chain.
        auto transport = now - lastSound < .6 ? model.tapeSpeed() : 0.f;
        for (const auto instance : order)
            if (instance >= 0 && juce::String(modules[static_cast<size_t>(instance) % modules.size()].prefix) == "stretch")
                if (const auto program = model.stretchPreview(instance); program.active && program.stretching)
                    transport *= 1.f / juce::jmax(1.f, program.current);
        for (auto& slot : slots)
        {
            slot->refresh();
            if (slot->drawer)
                slot->drawer->setTransport(transport);
        }
        if (inspector)
            inspector->refresh(model);
        std::vector<SignalChain::Entry> entries;
        for (const auto instance : order)
        {
            if (instance == noCartridge)
                break;
            const auto* power = instance >= 0 ? state.getRawParameterValue(effectPrefix(instance) + "active") : nullptr;
            entries.push_back({ instance, power != nullptr && power->load(std::memory_order_relaxed) >= .5f });
        }
        chain.update(std::move(entries));

        const auto id = model.presetIdentity();
        if (id == displayed && preset.getButtonText().isNotEmpty())
            return;
        displayed = id;
        selectedFile = juce::File{};
        juce::String name = "Custom";
        if (id.origin >= generatedOriginBase)
        {
            const auto generatedTag = id.origin - generatedOriginBase;
            if (id.variation >= 0)
                name = juce::String(generatedName(id.variation));
            else
                name = "Generated / " + juce::String(tagNames[static_cast<size_t>(generatedTag)]);
            tag = generatedTag;
        }
        else if (id.origin >= 0)
            name = presets()[static_cast<size_t>(id.origin)].name;
        preset.setButtonText(name);
    }

    // Opens the MOD drawer on a source's tab; asking for the tab already open, or for -1, closes it.
    void toggleDrawer(int source)
    {
        if (source < 0 || (modWindow.isOpen() && modWindow.drawer.page() == source))
        {
            modWindow.setOpen(false);
            master.setOpenSource(-1);
            return;
        }
        modWindow.open(source);
        master.setOpenSource(source);
    }

public:
    // Extra height the surface takes below the deck while the MOD drawer is out; the editor grows to show it.
    static constexpr int drawerSpace = ModDrawer::height + 6;
    std::function<void(int extra)> onExtraHeight;

private:
    // Grows or shrinks the surface below the deck to `extra` pixels and asks the editor to follow.
    void setDrawerSpace(int extra)
    {
        const auto height = editorDesignHeight + juce::jlimit(0, drawerSpace, extra);
        if (getHeight() == height)
            return;
        setSize(editorDesignWidth, height);
        if (onExtraHeight)
            onExtraHeight(height - editorDesignHeight);
    }

    void inspectCartridge(int instance)
    {
        const auto type = instance % modules.size();
        const auto prefix = effectPrefix(instance);
        // The panel grows out of the card it belongs to.
        Area origin;
        for (const auto& slot : slots)
            if (slot->instance == instance)
                origin = getLocalArea(&rack, slot->getBounds());
        inspect(modules[type].name, prefix, false, prefix.endsWith("delay_") ? instance : -1, origin,
                prefix.endsWith("stretch_") ? instance : -1);
        inspectedInstance = instance;
    }

    // `origin` is where the panel unfolds from, in this component's coordinates; the shared panels come out
    // of the options button.
    void inspect(const juce::String& title, const juce::String& prefix, bool global, int delayInstance = -1,
                 Area origin = {}, int stretchInstance = -1)
    {
        inspectedInstance = -1;
        inspector = std::make_unique<CartridgeInspector>(processor, state, title, prefix, global, art,
                                                          delayInstance, stretchInstance);
        inspector->refresh(model);
        inspector->close.onClick = [this]
        {
            if (inspector == nullptr)
                return;
            inspector->dismiss([safe = juce::Component::SafePointer<CartridgeSurface>(this)]
            {
                if (safe != nullptr)
                    safe->inspector.reset();
            });
        };
        // The window opens over the cables; they come over it only while one is carried.
        addAndMakeVisible(*inspector);
        inspector->sendLookAndFeelChange();
        resized();
        const auto from = origin.isEmpty() ? options.getBounds() : origin;
        inspector->open(from - inspector->getPosition());
    }

    static juce::File libraryDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("VST3 Presets/Tape Prototype/Tape Multi-FX");
    }

    static juce::PopupMenu libraryMenu(const juce::File& directory, juce::Array<juce::File>& files)
    {
        juce::PopupMenu menu;
        if (!directory.isDirectory() || directory.isSymbolicLink())
            return menu;
        auto entries = directory.findChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false, "*");
        entries.sort();
        for (const auto& entry : entries)
        {
            if (entry.isDirectory())
            {
                auto nested = libraryMenu(entry, files);
                if (nested.getNumItems() > 0)
                    menu.addSubMenu(entry.getFileName(), nested);
            }
            else if (entry.hasFileExtension("vstpreset"))
            {
                menu.addItem(1000 + files.size(), entry.getFileNameWithoutExtension());
                files.add(entry);
            }
        }
        return menu;
    }

    // The library's categories form the top level of the preset menu. A library kept inside a single wrapper
    // folder (as an import leaves it) is opened up so its categories still sit at the top.
    static juce::File libraryRoot()
    {
        auto root = libraryDirectory();
        const auto entries = root.findChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles,
                                                 false, "*");
        const auto folders = std::count_if(entries.begin(), entries.end(), [](const auto& f) { return f.isDirectory(); });
        const auto presetsHere = std::count_if(entries.begin(), entries.end(),
                                               [](const auto& f) { return f.hasFileExtension("vstpreset"); });
        if (folders == 1 && presetsHere == 0)
            for (const auto& entry : entries)
                if (entry.isDirectory())
                    return entry;
        return root;
    }

    void showPresets()
    {
        juce::PopupMenu menu, factory;
        const auto tick = [this](size_t i) { return displayed.origin == static_cast<int>(i); };
        menu.addItem(1, presets()[0].name, true, tick(0));
        menu.addSeparator();
        juce::Array<juce::File> files;
        auto library = libraryMenu(libraryRoot(), files);
        for (juce::PopupMenu::MenuItemIterator item(library); item.next();)
            menu.addItem(item.getItem());
        auto factoryChosen = false;
        for (size_t i = 1; i < presets().size(); ++i)
        {
            factory.addItem(static_cast<int>(i) + 1, presets()[i].name, true, tick(i));
            factoryChosen = factoryChosen || tick(i);
        }
        menu.addSubMenu("4. MELTIT", factory, true, nullptr, factoryChosen);
        menu.addSeparator();
        menu.addItem(999, "Import VST3 preset...");
        menu.setLookAndFeel(&look);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(preset),
            [safe = juce::Component::SafePointer<CartridgeSurface>(this), files = std::move(files)](int id)
            {
                if (safe == nullptr || id == 0)
                    return;
                if (id == 999)
                    safe->importPreset();
                else if (id >= 1000)
                    safe->loadFile(files[id - 1000]);
                else
                    safe->loadFactory(id - 1);
            });
    }

    void showOptions()
    {
        juce::PopupMenu menu, tags;
        menu.addItem(1, "Global controls");
        menu.addItem(2, "LFO 1");
        menu.addItem(3, "LFO 2");
        menu.addItem(4, "Macro");
        for (size_t i = 0; i < tagNames.size(); ++i)
            tags.addItem(100 + static_cast<int>(i), tagNames[i], true, tag == static_cast<int>(i));
        menu.addSubMenu("Generate style", tags);
        menu.addSeparator();
        menu.addItem(5, "Clear rack", !slots.empty());
        menu.setLookAndFeel(&look);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(options),
            [safe = juce::Component::SafePointer<CartridgeSurface>(this)](int id)
            {
                if (safe == nullptr || id == 0)
                    return;
                switch (id)
                {
                    case 1: safe->inspect("Global controls", {}, true); break;
                    case 2: safe->toggleDrawer(0); break;
                    case 3: safe->toggleDrawer(1); break;
                    case 4: safe->toggleDrawer(2); break;
                    case 5: safe->model.clearCartridges(); safe->refresh(); break;
                    default: safe->tag = id - 100; break;
                }
            });
    }

    void importPreset()
    {
        auto directory = selectedFile.existsAsFile() ? selectedFile.getParentDirectory() : libraryDirectory();
        chooser = std::make_unique<juce::FileChooser>("Import a Tape Multi-FX preset", directory, "*.vstpreset");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<CartridgeSurface>(this)](const juce::FileChooser& dialog)
            {
                if (safe != nullptr && dialog.getResult().existsAsFile())
                    safe->loadFile(dialog.getResult());
            });
    }

    void loadFile(const juce::File& file)
    {
        if (model.loadPresetFile(file))
        {
            inspector.reset();
            refresh();
            selectedFile = file;
            preset.setButtonText(file.getFileNameWithoutExtension());
        }
        else
            notice.show("Import failed", "Could not load this Tape Multi-FX preset.");
    }

    void loadFactory(int index)
    {
        selectedFile = juce::File{};
        preset.setButtonText({});
        inspector.reset();
        model.loadPreset(index);
        refresh();
    }

    void stepPreset(int direction)
    {
        if (selectedFile.existsAsFile())
        {
            auto files = selectedFile.getParentDirectory().findChildFiles(juce::File::findFiles, false, "*.vstpreset");
            files.sort();
            if (!files.isEmpty())
            {
                loadFile(files[(files.indexOf(selectedFile) + direction + files.size()) % files.size()]);
                return;
            }
        }
        const auto count = static_cast<int>(presets().size());
        const int index = displayed.origin >= 0 && displayed.origin < count ? displayed.origin
                                                                           : (direction > 0 ? count - 1 : 0);
        loadFactory((index + direction + count) % count);
    }

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    EditorModel& model;
    CartridgeImages art;
    CartridgeLook look;
    juce::TextButton previous { juce::String::charToString(0x2039) };
    juce::TextButton next { juce::String::charToString(0x203a) };
    juce::TextButton preset, generate { "Generate" }, options { juce::String::fromUTF8("•••") };
    HeaderBadge badge;
    RackTray rack;
    juce::Viewport viewport;
    RackEdgeShade edgeShade { viewport };
    ShelfStrip shelfStrip;
    juce::Viewport shelfView;
    std::array<ArtworkButton, editorModuleCount> shelf;
    ArtworkButton added;
    DragGhost ghost { art };
    std::vector<std::unique_ptr<FlyingCartridge>> flights;
    int landed = 0;
    bool dealing = false, dealPending = false;
    NoticePlate notice;
    int laidOutSlots = -1, laidOutWidth = -1;
    bool addMoving = false;
    juce::Rectangle<float> addFrom, addTo;
    double addStarted = 0;
    juce::VBlankAttachment addFrames { this, [this] { tickAdd(); } };
    SignalChain chain;
    PaintCache background;
    bool scrolling = false;
    int scrollFrom = 0, scrollTo = 0;
    double scrollStart = 0;
    juce::VBlankAttachment scrollFrames { this, [this] { tickScroll(); } };
    std::vector<std::unique_ptr<CartridgeSlot>> slots;
    std::unique_ptr<CartridgeInspector> inspector;
    int inspectedInstance = -1; // The card whose controls are open, or -1 for the shared panels.
    CartridgeOrder order;
    std::array<float, 2> peaks {};
    double lastSound = -10;
    Area tray, presetHousing, masterHousing;
    MasterStrip master { state };
    ModDrawerWindow modWindow { state };
    std::unique_ptr<PatchCables> cables;
    PresetIdentity displayed;
    juce::File selectedFile;
    std::unique_ptr<juce::FileChooser> chooser;
    int target = -1, tag = 0, animatePosition = -1;
    bool dragging = false;
};

class CartridgeEditor final : public juce::AudioProcessorEditor
{
public:
    CartridgeEditor(juce::AudioProcessor& processor, juce::AudioProcessorValueTreeState& parameters, EditorModel& model)
        : juce::AudioProcessorEditor(processor), surface(processor, parameters, model), sizeModel(model)
    {
        setOpaque(true);
        addAndMakeVisible(surface);
        setResizable(true, true);
        // The window grows downwards while the MOD drawer is out, keeping the plugin's scale.
        surface.onExtraHeight = [this](int extra) { useDesignHeight(editorDesignHeight + extra); };
        useDesignHeight(editorDesignHeight);
        // Reopen at the size the user left it at.
        const auto scale = juce::jlimit(.8f, 1.5f, sizeModel.editorScale());
        setSize(juce::roundToInt(editorDesignWidth * scale), juce::roundToInt(editorDesignHeight * scale));
        constructed = true;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(charcoal);
    }

    void resized() override
    {
        if (getWidth() == 0 || getHeight() == 0)
            return;

        if (constructed)
            sizeModel.setEditorScale(static_cast<float>(getWidth()) / editorDesignWidth);
        const auto scale = juce::jmin(static_cast<float>(getWidth()) / editorDesignWidth,
                                     static_cast<float>(getHeight()) / static_cast<float>(designHeight));
        const auto x = (static_cast<float>(getWidth()) - editorDesignWidth * scale) * .5f;
        const auto y = (static_cast<float>(getHeight()) - static_cast<float>(designHeight) * scale) * .5f;
        // JUCE applies the host DPI scale to the outer AudioProcessorEditor.
        surface.setTransform(juce::AffineTransform::scale(scale).translated(x, y));
    }

private:
    CartridgeSurface surface;
    EditorModel& sizeModel;
    bool constructed = false;
    int designHeight = editorDesignHeight;

    void useDesignHeight(int height)
    {
        const auto scale = getWidth() > 0 ? static_cast<float>(getWidth()) / editorDesignWidth : 1.f;
        designHeight = height;
        // Free the proportions while the height changes, so the width stays where it is.
        getConstrainer()->setFixedAspectRatio(0.0);
        setResizeLimits(editorDesignWidth * 4 / 5, height * 4 / 5, editorDesignWidth * 3 / 2, height * 3 / 2);
        if (getWidth() > 0)
            setSize(getWidth(), juce::roundToInt(static_cast<float>(height) * scale));
        getConstrainer()->setFixedAspectRatio(static_cast<double>(editorDesignWidth) / height);
    }

    void parentHierarchyChanged() override
    {
        if (getAudioProcessor()->wrapperType == juce::AudioProcessor::wrapperType_Standalone)
            if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                window->setName("MELTIT");
    }
};
}

juce::AudioProcessorEditor* makeCartridgeEditor(juce::AudioProcessor& processor,
                                                juce::AudioProcessorValueTreeState& state, EditorModel& model)
{
    return new CartridgeEditor(processor, state, model);
}
}
