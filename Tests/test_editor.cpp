#include "PresetFile.h"
#include "RackEditor.h"
#include "RackPresets.h"
#include "RetroArt.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

#if JUCE_WINDOWS
 #include <Windows.h>
 #undef min
 #undef max
#elif JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
void check(bool condition, const char* failure)
{
    if (!condition)
        throw std::runtime_error(failure);
}

auto openEditor(juce::AudioProcessor& processor)
{
    const auto close = [&processor](juce::AudioProcessorEditor* editor)
    {
        processor.editorBeingDeleted(editor);
        delete editor;
    };
    auto* editor = processor.createEditorAndMakeActive();
    check(editor != nullptr, "The processor did not open its editor");
    return std::unique_ptr<juce::AudioProcessorEditor, decltype(close)>(editor, close);
}

juce::Component* find(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id)
        return &root;
    for (auto* child : root.getChildren())
        if (auto* match = find(*child, id))
            return match;
    return nullptr;
}

template <typename Type>
Type& control(juce::Component& root, const juce::String& id)
{
    if (auto* match = dynamic_cast<Type*>(find(root, id)))
        return *match;
    throw std::runtime_error("Missing control: " + id.toStdString());
}

juce::RangedAudioParameter& parameter(juce::AudioProcessor& processor, const juce::String& id)
{
    for (auto* item : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(item); ranged && ranged->paramID == id)
            return *ranged;
    throw std::runtime_error("Missing parameter: " + id.toStdString());
}

float value(juce::AudioProcessor& processor, const juce::String& id)
{
    auto& ranged = parameter(processor, id);
    return ranged.convertFrom0to1(ranged.getValue());
}

void click(juce::Component& editor, const char* id)
{
    auto& button = control<juce::TextButton>(editor, id);
    check(button.isEnabled() && static_cast<bool>(button.onClick), "Button cannot be used");
    button.onClick();
}

void drag(juce::Component& editor, const char* id, juce::Point<int> destination)
{
    auto& button = control<juce::Component>(editor, id);
    const auto down = button.getLocalBounds().getCentre().toFloat();
    const auto end = button.getLocalPoint(&editor, destination).toFloat();
    const auto now = juce::Time::getCurrentTime();
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    auto event = [&](juce::Point<float> at, juce::ModifierKeys modifier, bool moved)
    {
        return juce::MouseEvent(mouse, at, modifier, 0.f, 0.f, 0.f, 0.f, 0.f, &button, &button,
                                now, down, now, 1, moved);
    };
    const juce::ModifierKeys held(juce::ModifierKeys::leftButtonModifier);
    button.mouseDown(event(down, held, false));
    button.mouseDrag(event(end, held, true));
    button.mouseUp(event(end, {}, true));
}

// Presses at `from` and drags to `to`, both in the component's own coordinates.
void dragWithin(juce::Component& target, juce::Point<float> from, juce::Point<float> to)
{
    const auto now = juce::Time::getCurrentTime();
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    auto event = [&](juce::Point<float> at, juce::ModifierKeys modifier, bool moved)
    {
        return juce::MouseEvent(mouse, at, modifier, 0.f, 0.f, 0.f, 0.f, 0.f, &target, &target,
                                now, from, now, 1, moved);
    };
    const juce::ModifierKeys held(juce::ModifierKeys::leftButtonModifier);
    target.mouseDown(event(from, held, false));
    target.mouseDrag(event(to, held, true));
    target.mouseUp(event(to, {}, true));
}

void snapshot(juce::Component& editor, const char* path)
{
    juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    auto stream = file.createOutputStream();
    check(stream && stream->openedOk(), "Could not open editor preview path");
    stream->setPosition(0);
    stream->truncate();
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    juce::PNGImageFormat format;
    check(format.writeImageToStream(image, *stream), "Could not render editor preview");
}
}

namespace
{
void dispatch()
{
#if JUCE_WINDOWS
    // Process the message-thread drop callback and the drawer animation.
    for (int i = 0; i < 8; ++i)
    {
        MSG message {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        juce::Thread::sleep(12);
    }
#else
    // Same on macOS: run the main run loop, which carries the drop callback and the drawer animation.
    // (MessageManager::runDispatchLoopUntil is compiled out unless JUCE_MODAL_LOOPS_PERMITTED is set.)
   #if JUCE_MAC
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.12, false);
   #endif
#endif
    juce::Timer::callPendingTimersSynchronously();
}

juce::Point<int> centre(juce::Component& editor, const char* id)
{
    auto& component = control<juce::Component>(editor, id);
    return editor.getLocalPoint(&component, component.getLocalBounds().getCentre());
}

juce::String presetName(juce::Component& editor)
{
    return control<juce::TextButton>(editor, "presets").getButtonText();
}

void set(juce::AudioProcessor& processor, const juce::String& id, float amount)
{
    auto& setting = parameter(processor, id);
    setting.setValueNotifyingHost(setting.convertTo0to1(amount));
}

juce::MemoryBlock save(juce::AudioProcessor& processor)
{
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    return block;
}

void restore(juce::AudioProcessor& processor, const juce::MemoryBlock& block)
{
    processor.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
}

std::array<float, 16000> impulse(juce::AudioProcessor& processor)
{
    processor.prepareToPlay(48000, 256);
    std::array<float, 16000> response {};
    juce::AudioBuffer<float> block(2, 256);
    juce::MidiBuffer midi;
    for (int start = 0; start < static_cast<int>(response.size()); start += block.getNumSamples())
    {
        block.clear();
        if (start == 0)
        {
            block.setSample(0, 0, .5f);
            block.setSample(1, 0, .5f);
        }
        processor.processBlock(block, midi);
        std::copy_n(block.getReadPointer(0), std::min(block.getNumSamples(), static_cast<int>(response.size()) - start),
                    response.begin() + start);
    }
    processor.releaseResources();
    return response;
}

float echo(const std::array<float, 16000>& response, int milliseconds)
{
    const auto sample = milliseconds * 48;
    float level = 0;
    for (int i = sample - 40; i <= sample + 200; ++i)
        level = std::max(level, std::abs(response[i]));
    return level;
}

void checkFactory(juce::Component& editor, juce::AudioProcessor& processor)
{
    check(control<juce::TextButton>(editor, "previous_preset").getButtonText() == juce::String::charToString(0x2039)
              && control<juce::TextButton>(editor, "next_preset").getButtonText() == juce::String::charToString(0x203a),
          "Preset arrow glyphs are incorrect");
    for (const auto& preset : rack::presets())
    {
        click(editor, "next_preset");
        check(presetName(editor) == preset.name, "Next arrow did not load the expected preset");
        for (const auto& setting : preset.values)
            check(std::abs(value(processor, setting.id) - setting.value) < .01f, "Preset parameter did not load");
    }
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Next arrow did not wrap");
    click(editor, "previous_preset");
    check(presetName(editor) == rack::presets().back().name, "Previous arrow did not wrap");
    click(editor, "next_preset");
    check(find(editor, "slot_0") == nullptr, "Initialize did not clear the rack");
}

void checkReadout(juce::Component& editor, const char* id)
{
    auto& slider = control<juce::Slider>(editor, id);
    auto& display = control<juce::Label>(slider, juce::String(id) + "_value");
    const auto layout = slider.getLookAndFeel().getSliderLayout(slider);
    check(slider.getLocalBounds().contains(display.getBounds()), "Value display is clipped");
    check(layout.sliderBounds.getBottom() + 4 <= display.getY(), "Knob intersects its value display");
    check(display.getWidth() >= 80 && display.getHeight() >= 21, "Value display is too short");
}

void checkPlus(juce::Component& editor)
{
    auto& button = control<juce::TextButton>(editor, "add_slot");
    const auto& viewport = control<juce::Viewport>(editor, "rack_viewport");
    const auto& surface = *viewport.getParentComponent();
    const auto bounds = surface.getLocalArea(&button, button.getLocalBounds());
    const auto rack = viewport.getBounds().withWidth(surface.getWidth() - 2 * viewport.getX());
    check(button.isVisible() && button.getButtonText() == "+", "Add slot is missing its plus-only face");
    check(rack.contains(bounds) && bounds.getWidth() == 68, "Add slot is clipped by the rack");
    check(bounds.getHeight() > 300 && bounds.getBottom() >= rack.getBottom() - viewport.getScrollBarThickness() - 8,
          "Add slot does not run the full height of the rack");
}

void checkShelf(juce::Component& editor)
{
    float edge = 0.f;
    for (const auto* id : { "tape_cartridge", "tremolo_cartridge", "saturation_cartridge", "resampler_cartridge",
                            "chorus_cartridge", "delay_cartridge", "reverse_cartridge", "reverb_cartridge",
                            "stretch_cartridge", "vhs_cartridge", "grain_cartridge" })
    {
        auto& card = control<juce::Component>(editor, id);
        const auto bounds = editor.getLocalArea(&card, card.getLocalBounds().toFloat());
        const auto aspect = static_cast<float>(card.getWidth()) / card.getHeight();
        // Shelf cartridges are the same cartridges that seat in the rack, so they keep the seated proportions.
        check(aspect >= 1.95f && aspect <= 2.2f, "The cartridge is stretched out of its seated proportions");
        // The shelf scrolls sideways, so a card may lie past the window's edge, but never across its neighbour
        // and never outside the strip that carries it.
        check(card.getHeight() >= 70, "Shelf cartridges are too small to read");
        check(bounds.getX() >= edge && card.getParentComponent()->getLocalBounds().contains(card.getBounds()),
              "A cartridge overlaps its neighbour or is clipped");
        edge = bounds.getRight();
    }
    const auto& shelf = control<juce::Viewport>(editor, "shelf_viewport");
    check(shelf.getViewedComponent() != nullptr
              && (shelf.getViewedComponent()->getWidth() <= shelf.getViewWidth() || shelf.isHorizontalScrollBarShown()),
          "The shelf hides cartridges without a scroll bar to reach them");
}

// The cartridge label names the effect, so cards carry no title: the power lamp and the key to the full
// controls sit on the dock bar beside eject, and a quick selector, where a card has one, is centred in the
// card's header strip. Returns the lamp and key in slot coordinates, which every card shares.
juce::Rectangle<int> checkCompactHeading(juce::Component& editor, const juce::String& prefix,
                                         const char* selectorSuffix = nullptr)
{
    auto& powerLamp = control<juce::Component>(editor, prefix + "power");
    auto& detailsKey = control<juce::Component>(editor, prefix + "controls");
    check(find(editor, prefix + "title") == nullptr, "Cards must not repeat the cartridge's name as a title");
    const auto& rackSlot = *powerLamp.getParentComponent();
    check(detailsKey.getParentComponent() == &rackSlot, "Lamp and controls key must share the dock bar");
    const auto siblings = rackSlot.getChildren();
    const auto ejectKey = std::find_if(siblings.begin(), siblings.end(), [](const auto* child)
    {
        return child->getComponentID().startsWith("eject_");
    });
    check(ejectKey != siblings.end(), "Compact effect has no eject key");
    const auto power = powerLamp.getBounds();
    const auto details = detailsKey.getBounds();
    const auto eject = (*ejectKey)->getBounds();
    check(power.getCentreY() == eject.getCentreY() && details.getCentreY() == eject.getCentreY(),
          "Lamp, controls key and eject must share one centerline on the dock bar");
    check(power.getRight() < details.getX() && details.getRight() < eject.getX(), "Dock bar keys overlap");

    if (selectorSuffix != nullptr)
    {
        auto& selectorControl = control<juce::Component>(editor, prefix + selectorSuffix);
        const auto& drawer = *selectorControl.getParentComponent();
        const auto selector = selectorControl.getBounds();
        const auto slot = std::find_if(siblings.begin(), siblings.end(), [](const auto* child)
        {
            return child->getComponentID().startsWith("slot_");
        });
        check(slot != siblings.end(), "Compact effect has no mounted cartridge");
        const auto mountBottom = drawer.getLocalArea(*slot, (*slot)->getLocalBounds()).getBottom();
        const auto divider = juce::roundToInt(drawer.getHeight() * 90.f / 513.f);
        check(std::abs(selector.getCentreY() * 2 - (mountBottom + divider)) <= 1
                  && std::abs(selector.getCentreX() * 2 - drawer.getWidth()) <= 1,
              "Compact selector must be centred in the card's header strip");
    }
    return power.getUnion(details);
}

void checkUniformScaling(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    const auto editor = openEditor(*processor);
    click(*editor, "next_preset");
    check(presetName(*editor) == "Initialize", "Could not reset the rack before testing the four slots");
    for (const auto* cartridge : { "delay_cartridge", "tape_cartridge", "chorus_cartridge", "reverb_cartridge" })
    {
        drag(*editor, cartridge, centre(*editor, "add_slot"));
        dispatch();
    }

    const auto baseSize = editor->getLocalBounds().getBottomRight();
    auto& viewport = control<juce::Viewport>(*editor, "rack_viewport");
    for (const auto* id : { "slot_0", "slot_1", "slot_2", "slot_3", "add_slot" })
    {
        auto& part = control<juce::Component>(*editor, id);
        check(viewport.getLocalBounds().contains(viewport.getLocalArea(&part, part.getLocalBounds())),
              "Four complete slots and the plus must fit in the rack");
    }
    set(*processor, "delay_note", 2);
    set(*processor, "delay_timing", 2);
    auto& chorusMode = control<juce::ComboBox>(*editor, "chorus_quick_mode");
    chorusMode.setSelectedId(1, juce::sendNotificationSync);
    check(value(*processor, "chorus_stereo") == 0, "Compact Chorus mode missed the parameter");
    chorusMode.setSelectedId(2, juce::sendNotificationSync);
    check(value(*processor, "chorus_stereo") == 1, "Compact Chorus mode did not restore stereo");
    auto& reverbMode = control<juce::ComboBox>(*editor, "reverb_quick_mode");
    reverbMode.setSelectedId(3, juce::sendNotificationSync);
    check(value(*processor, "reverb_type") == 2, "Compact Reverb mode missed the parameter");
    for (int i = 0; i < 6; ++i)
        dispatch();
    const auto sharedHeading = checkCompactHeading(*editor, "delay_", "division");
    for (const auto* prefix : { "tape_", "chorus_", "reverb_" })
        check(checkCompactHeading(*editor, prefix, "quick_mode") == sharedHeading,
              "Every card must mount its lamp and controls key in the same place");
    if (argc >= 6)
        snapshot(*editor, argv[5]);

    click(*editor, "eject_2");
    dispatch();
    drag(*editor, "saturation_cartridge", centre(*editor, "add_slot"));
    dispatch();
    drag(*editor, "slot_3", centre(*editor, "slot_2"));
    for (int i = 0; i < 6; ++i)
        dispatch();
    click(*editor, "saturation_controls");
    check(find(*editor, "saturation_mode") != nullptr && find(*editor, "saturation_unstable") != nullptr,
          "The expanded Saturation panel is missing controls");
    if (argc >= 7)
        snapshot(*editor, argv[6]);

    constexpr std::array<const char*, 8> sample {
        "previous_preset", "slot_3", "add_slot", "reverb_cartridge",
        "delay_power", "delay_division", "close_controls", "saturation_unstable"
    };
    auto renderedBounds = [&](const char* id)
    {
        auto& part = control<juce::Component>(*editor, id);
        return editor->getLocalArea(&part, part.getLocalBounds().toFloat());
    };

    std::array<juce::Rectangle<float>, sample.size()> baseline;
    for (size_t i = 0; i < sample.size(); ++i)
        baseline[i] = renderedBounds(sample[i]);

    for (const auto zoom : { .8f, 1.25f, 1.5f })
    {
        editor->setSize(juce::roundToInt(baseSize.x * zoom), juce::roundToInt(baseSize.y * zoom));
        const auto scale = juce::jmin(static_cast<float>(editor->getWidth()) / baseSize.x,
                                     static_cast<float>(editor->getHeight()) / baseSize.y);
        const auto x = (editor->getWidth() - baseSize.x * scale) * .5f;
        const auto y = (editor->getHeight() - baseSize.y * scale) * .5f;
        const auto transform = juce::AffineTransform::scale(scale).translated(x, y);
        for (size_t i = 0; i < sample.size(); ++i)
        {
            const auto actual = renderedBounds(sample[i]);
            const auto expected = baseline[i].transformedBy(transform);
            check(actual.getTopLeft().getDistanceFrom(expected.getTopLeft()) < .05f
                      && actual.getBottomRight().getDistanceFrom(expected.getBottomRight()) < .05f,
                  "A component did not follow the shared plugin scale");
        }
        checkPlus(*editor);
        checkShelf(*editor);
        if (zoom == 1.25f && argc >= 8)
            snapshot(*editor, argv[7]);
    }

    click(*editor, "close_controls");
    dispatch();
    drag(*editor, "slot_3", centre(*editor, "slot_1"));
    dispatch();
    check(centre(*editor, "reverb_power").x < centre(*editor, "tape_power").x,
          "Reordering a cartridge failed after scaling the entire GUI");

    if (argc >= 9)
    {
        editor->setSize(baseSize.x, baseSize.y);
        for (const auto* id : { "eject_3", "eject_2", "eject_1", "eject_0" })
        {
            click(*editor, id);
            dispatch();
        }

        for (const auto* id : { "tremolo_cartridge", "saturation_cartridge", "resampler_cartridge", "reverse_cartridge" })
        {
            drag(*editor, id, centre(*editor, "add_slot"));
            dispatch();
        }
        for (int i = 0; i < 6; ++i)
            dispatch();
        checkPlus(*editor);

        for (const auto* prefix : { "tremolo_", "saturation_", "resampler_", "reverse_" })
            check(checkCompactHeading(*editor, prefix) == sharedHeading,
                  "Compact cards without selectors must use the same title column");
        snapshot(*editor, argv[8]);
    }

    editor->setSize(baseSize.x, baseSize.y);
    drag(*editor, "chorus_cartridge", centre(*editor, "add_slot"));
    dispatch();
    check(find(*editor, "slot_4") != nullptr, "The fifth cartridge was not inserted");
    auto& plus = control<juce::TextButton>(*editor, "add_slot");
    viewport.setViewPosition(0, 0);
    checkPlus(*editor);
    const auto pinnedBounds = plus.getBounds();
    check(pinnedBounds.getX() > viewport.getRight(), "The add button overlaps the scrolled cards");
    viewport.setViewPosition(viewport.getViewedComponent()->getWidth(), 0);
    check(viewport.getViewPositionX() > 0 && plus.getBounds() == pinnedBounds,
          "The add button moved when the cards scrolled");

    // Clicking the first plate on the signal line brings the first cartridge back into view.
    {
        auto& chainBar = control<juce::Component>(*editor, "signal_chain");
        const auto now = juce::Time::getCurrentTime();
        const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
        const juce::Point<float> first(chainBar.getWidth() / 10.f, chainBar.getHeight() / 2.f);
        chainBar.mouseUp(juce::MouseEvent(mouse, first, {}, 0.f, 0.f, 0.f, 0.f, 0.f, &chainBar, &chainBar,
                                          now, first, now, 1, false));
        check(viewport.getViewPositionX() == 0, "Clicking a chain plate did not scroll the rack to its cartridge");
        viewport.setViewPosition(viewport.getViewedComponent()->getWidth(), 0);
    }
    checkPlus(*editor);
    if (argc >= 10)
        snapshot(*editor, argv[9]);

    editor->setSize(juce::roundToInt(baseSize.x * 1.25f), juce::roundToInt(baseSize.y * 1.25f));
    const auto plusRight = editor->getLocalPoint(&plus, juce::Point<int>(plus.getWidth() - 6, plus.getHeight() / 2));
    drag(*editor, "tape_cartridge", plusRight);
    dispatch();
    check(find(*editor, "slot_5") != nullptr, "The pinned plus did not accept a drop at 125 percent scale");
    checkPlus(*editor);
    click(*editor, "add_slot");
    dispatch();
    check(find(*editor, "slot_6") != nullptr, "Clicking the pinned plus did not add an empty slot");
    checkPlus(*editor);

    for (const auto* id : { "eject_6", "eject_5", "eject_4" })
    {
        click(*editor, id);
        dispatch();
    }
    check(viewport.getViewPositionX() == 0 && plus.getRight() <= viewport.getRight(),
          "The add button did not return next to the last card after shortening the row");
}

// Every card keeps its four controls apart, inside the card and above the trim band along its bottom.
// Cards with a header strip keep their controls below it.
void checkCardLayouts()
{
    for (size_t effect = 0; effect < rack::editorModuleCount; ++effect)
    {
        const auto top = rack::retro::cardHasHeader(effect) ? 44 : 16;
        const juce::Rectangle<int> controls(0, top, 240, 226 - top);
        const auto layout = rack::retro::cardLayout(effect);
        const auto screen = rack::retro::cardScreen(effect);
        check(screen.isEmpty() || controls.contains(screen), "A card screen reaches outside the control area");
        for (size_t i = 0; i < layout.size(); ++i)
        {
            if (layout[i].bounds.isEmpty())
                continue;
            check(controls.contains(layout[i].bounds), "A card control reaches into the card's bottom trim band");
            check(!layout[i].bounds.intersects(screen), "A card control overlaps the card's screen");
            for (size_t j = i + 1; j < layout.size(); ++j)
                check(!layout[i].bounds.intersects(layout[j].bounds), "Two controls overlap on an effect card");
        }
    }
}
}

// The VHS cartridge seats like any other, shows its display and three knobs, and degrades the audio through it:
// the band narrows and the result stays finite and close in level.
void checkVhsCard(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before testing VHS");
    drag(editor, "tape_cartridge", centre(editor, "add_slot"));
    dispatch();
    drag(editor, "vhs_cartridge", centre(editor, "add_slot"));
    dispatch();
    for (const auto* id : { "vhs_wear", "vhs_tracking", "vhs_wobble", "vhs_quick_mode", "vhs_screen" })
        check(find(editor, id) != nullptr, (juce::String("The VHS card is missing ") + id).toRawUTF8());
    check(value(processor, "vhs_active") == 1, "Inserting the VHS cartridge did not power it");
    auto& speed = control<juce::ComboBox>(editor, "vhs_quick_mode");
    speed.setSelectedId(3, juce::sendNotificationSync);
    check(value(processor, "vhs_speed") == 2, "The VHS speed selector missed the parameter");

    // A bright 9 kHz tone through EP speed comes out well below a 300 Hz tone.
    const auto level = [&processor](float hertz)
    {
        processor.prepareToPlay(48000, 480);
        juce::AudioBuffer<float> audio(2, 480);
        juce::MidiBuffer midi;
        float peak = 0;
        bool finite = true;
        for (int block = 0; block < 100; ++block)
        {
            for (int i = 0; i < 480; ++i)
                audio.setSample(0, i, .25f * std::sin(juce::MathConstants<float>::twoPi * hertz
                                                      * static_cast<float>(block * 480 + i) / 48000.f));
            audio.copyFrom(1, 0, audio, 0, 0, 480);
            processor.processBlock(audio, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 480; ++i)
                    finite = finite && std::isfinite(audio.getSample(c, i));
            if (block >= 50)
                peak = std::max(peak, audio.getMagnitude(0, 0, 480));
        }
        processor.releaseResources();
        check(finite, "The VHS path produced a non-finite sample");
        return peak;
    };
    set(processor, "tape_active", 0);
    const auto low = level(300), high = level(9000);
    check(low > .12f && low < .45f, "The VHS path changed the level of a low tone too much");
    check(high < low * .25f, "EP speed did not roll off the highs");
    speed.setSelectedId(1, juce::sendNotificationSync);
    set(processor, "tape_active", 1);
    for (int i = 0; i < 4; ++i)
        dispatch();
    if (argc >= 15)
        snapshot(editor, argv[14]);
}

// The Grain cartridge seats like any other with its cloud screen, scale selector and four knobs. Through it a
// tone stays finite and close in level, a dry mix leaves it untouched, a pitch of one octave doubles its frequency
// and full feedback stays bounded.
void checkGrainCard(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before testing Grain");
    drag(editor, "grain_cartridge", centre(editor, "add_slot"));
    dispatch();
    for (const auto* id : { "grain_size", "grain_density", "grain_pitch", "grain_mix", "grain_quick_mode", "grain_screen" })
        check(find(editor, id) != nullptr, (juce::String("The Grain card is missing ") + id).toRawUTF8());
    check(value(processor, "grain_active") == 1, "Inserting the Grain cartridge did not power it");
    auto& scale = control<juce::ComboBox>(editor, "grain_quick_mode");
    scale.setSelectedId(4, juce::sendNotificationSync);
    check(value(processor, "grain_scale") == 3, "The Grain scale selector missed the parameter");
    scale.setSelectedId(2, juce::sendNotificationSync);

    // The screen is the XY pad: dragging left and up moves the X and Y macros, not the knobs under them, and a
    // double click centres them again.
    auto& screen = control<juce::Component>(editor, "grain_screen");
    const auto box = screen.getLocalBounds().toFloat();
    dragWithin(screen, box.getCentre(), { box.getX() + box.getWidth() * .25f, box.getY() + box.getHeight() * .3f });
    check(value(processor, "xy_x_amount") < 35 && value(processor, "xy_y_amount") > 65,
          "Dragging the Grain screen did not move the XY macros");
    check(value(processor, "grain_position") == 20 && value(processor, "grain_pitch") == 0,
          "Dragging the XY pad moved the knobs themselves");
    check(juce::roundToInt(value(processor, "xy_x_target_1")) == static_cast<int>(rack::grainPositionTarget)
              && juce::roundToInt(value(processor, "xy_y_target_1")) == static_cast<int>(rack::grainPitchTarget),
          "Dragging an unpatched XY pad did not patch it to Position and Pitch");
    set(processor, "xy_x_amount", 50);
    set(processor, "xy_y_amount", 50);

    // Runs a tone through the rack and returns the output peak over the second half, with the strength of the
    // output at `probe` Hz relative to the input frequency.
    struct Result { float peak = 0, ratio = 0; };
    const auto run = [&processor](float hertz, float probe)
    {
        processor.prepareToPlay(48000, 480);
        juce::AudioBuffer<float> audio(2, 480), input(2, 480);
        juce::MidiBuffer midi;
        Result result;
        bool finite = true;
        double atTone = 0, atProbe = 0;
        float largest = 0;
        for (int block = 0; block < 200; ++block)
        {
            for (int i = 0; i < 480; ++i)
                audio.setSample(0, i, .25f * std::sin(juce::MathConstants<float>::twoPi * hertz
                                                      * static_cast<float>(block * 480 + i) / 48000.f));
            audio.copyFrom(1, 0, audio, 0, 0, 480);
            input.makeCopyOf(audio);
            processor.processBlock(audio, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 480; ++i)
                    finite = finite && std::isfinite(audio.getSample(c, i));
            largest = std::max(largest, audio.getMagnitude(0, 480));
            if (block >= 100)
            {
                result.peak = std::max(result.peak, audio.getMagnitude(0, 0, 480));
                // Correlate against both frequencies, in quadrature, to measure their strength.
                for (const auto& [frequency, sum] : { std::pair { hertz, &atTone }, std::pair { probe, &atProbe } })
                {
                    double re = 0, im = 0;
                    for (int i = 0; i < 480; ++i)
                    {
                        const auto phase = juce::MathConstants<double>::twoPi * frequency * (block * 480 + i) / 48000.0;
                        re += audio.getSample(0, i) * std::cos(phase);
                        im += audio.getSample(0, i) * std::sin(phase);
                    }
                    *sum += std::sqrt(re * re + im * im);
                }
            }
        }
        processor.releaseResources();
        check(finite, "The Grain path produced a non-finite sample");
        check(largest < 4.f, "The Grain path ran away");
        result.ratio = static_cast<float>(atProbe / juce::jmax(1e-9, atTone));
        return result;
    };

    set(processor, "grain_mix", 0);
    const auto dry = run(300, 600);
    check(std::abs(dry.peak - .25f) < .02f, "A dry Grain mix changed the signal");
    set(processor, "grain_mix", 100);
    const auto cloud = run(300, 600);
    check(cloud.peak > .05f && cloud.peak < .8f, "The Grain cloud is far from the input level");
    set(processor, "grain_pitch", 12);
    const auto octave = run(300, 600);
    check(octave.ratio > 2.f, "A Grain pitch of twelve chromatic steps did not move the tone up an octave");
    set(processor, "grain_pitch", 0);
    set(processor, "grain_feedback", 100);
    set(processor, "grain_density", 40);
    set(processor, "grain_pitch", -12);
    set(processor, "grain_voices", 16);
    run(300, 600);
    set(processor, "grain_stretch", 1);
    set(processor, "grain_reverse", 1);
    set(processor, "grain_pitch_random", 60);
    run(300, 600);

    // Leave a lively cloud running for the picture, with the handle moved off centre.
    set(processor, "grain_position", 250);
    set(processor, "grain_pitch", 5);
    set(processor, "grain_feedback", 30);
    set(processor, "grain_stretch", 0);
    set(processor, "grain_reverse", 0);
    set(processor, "grain_width", 90);
    set(processor, "grain_size_random", 40);
    processor.prepareToPlay(48000, 480);
    {
        juce::AudioBuffer<float> audio(2, 480);
        juce::MidiBuffer midi;
        for (int block = 0; block < 40; ++block)
        {
            for (int i = 0; i < 480; ++i)
                audio.setSample(0, i, .25f * std::sin(juce::MathConstants<float>::twoPi * 300.f
                                                      * static_cast<float>(block * 480 + i) / 48000.f));
            audio.copyFrom(1, 0, audio, 0, 0, 480);
            processor.processBlock(audio, midi);
        }
    }
    for (int i = 0; i < 4; ++i)
        dispatch();
    if (argc >= 20)
        snapshot(editor, argv[19]);
    // Cables to knobs that live only in the card's full controls plug into its controls key; one to a card that is
    // not in the rack hangs above its own jack.
    set(processor, "xy_x_target_1", static_cast<float>(rack::grainVoicesTarget));
    set(processor, "xy_x_depth_1", 40);
    set(processor, "xy_x_active_1", 1);
    set(processor, "xy_y_target_1", static_cast<float>(rack::grainWindowTarget));
    set(processor, "xy_y_depth_1", -30);
    set(processor, "xy_y_active_1", 1);
    set(processor, "xy_y_target_2", static_cast<float>(rack::reverbMixTarget));
    set(processor, "xy_y_depth_2", 30);
    set(processor, "xy_y_active_2", 1);
    for (int i = 0; i < 4; ++i)
        dispatch();
    if (argc >= 23)
        snapshot(editor, argv[22]);
    // A cable carried from a jack and held on the card's input opens its controls; it is still in the hand and goes
    // into a knob there.
    {
        auto& cables = control<juce::Component>(editor, "patch_cables");
        const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
        const auto from = cables.getLocalPoint(&editor, centre(editor, "lfo_1_jack")).toFloat();
        const auto key = cables.getLocalPoint(&editor, centre(editor, "grain_controls")).toFloat();
        const auto event = [&](juce::Point<float> at, juce::ModifierKeys modifier)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent(mouse, at, modifier, 0.f, 0.f, 0.f, 0.f, 0.f, &cables, &cables, now, from, now, 1, true);
        };
        const juce::ModifierKeys held(juce::ModifierKeys::leftButtonModifier);
        cables.mouseDown(event(from, held));
        cables.mouseDrag(event(key, held));
        juce::Thread::sleep(350);
        cables.mouseDrag(event(key.translated(1, 0), held));
        for (int i = 0; i < 30; ++i)
            dispatch();
        auto* panel = find(editor, "effects_inspector");
        check(panel != nullptr, "Holding a cable on the Grain card's input did not open its controls");
        auto* size = dynamic_cast<juce::Slider*>(find(*panel, "grain_size_random"));
        check(size != nullptr, "The Grain controls window has no Size Random knob");
        const auto spot = cables.getLocalPoint(size, juce::Point<int>(size->getWidth() / 2, (size->getHeight() - 28) / 2)).toFloat();
        cables.mouseDrag(event(spot, held));
        // Over the window the carried cable runs through the card's input and out of the window's IN socket.
        if (argc >= 24)
            snapshot(editor, argv[23]);
        cables.mouseUp(event(spot, {}));
        check(juce::roundToInt(value(processor, "lfo_1_target_1")) == static_cast<int>(rack::grainSizeRandomTarget),
              "The cable carried into the opened controls did not patch the knob it was dropped on");
        set(processor, "lfo_1_target_1", 0);
        for (int i = 0; i < 4; ++i)
            dispatch();
    }
    if (find(editor, "effects_inspector") == nullptr)
        click(editor, "grain_controls");
    for (int i = 0; i < 30; ++i)
        dispatch();
    // In the open controls window a cable from a jack goes straight into one of its knobs.
    {
        auto& cables = control<juce::Component>(editor, "patch_cables");
        juce::Slider* feedback = nullptr;
        for (auto* found = find(editor, "effects_inspector"); found != nullptr && feedback == nullptr; found = nullptr)
            feedback = dynamic_cast<juce::Slider*>(find(*found, "grain_feedback"));
        check(feedback != nullptr, "The Grain controls window has no Feedback knob");
        const auto spot = editor.getLocalPoint(feedback, juce::Point<int>(feedback->getWidth() / 2, (feedback->getHeight() - 28) / 2));
        dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "lfo_1_jack")).toFloat(), cables.getLocalPoint(&editor, spot).toFloat());
        check(juce::roundToInt(value(processor, "lfo_1_target_1")) == static_cast<int>(rack::grainFeedbackTarget),
              "A cable dropped on a knob in the controls window did not patch it");
        for (int i = 0; i < 4; ++i)
            dispatch();
    }
    for (const auto* id : { "grain_voices", "grain_window", "grain_feedback", "grain_stretch_speed", "grain_retrigger",
                            "grain_division", "grain_sync", "grain_stretch", "grain_reverse" })
    {
        auto* found = find(editor, id);
        check(found != nullptr && found->isVisible(), (juce::String("The Grain controls panel is missing ") + id).toRawUTF8());
    }
    if (argc >= 21)
        snapshot(editor, argv[20]);
    for (const auto* id : { "xy_x_target_1", "xy_y_target_1", "xy_y_target_2", "lfo_1_target_1" })
        set(processor, id, 0);
    processor.releaseResources();
}

// Runs a 300 Hz tone through the processor for `blocks` blocks of 480 samples and returns the last block's peak
// alongside how far the output strays from the input in that block. `midi` is sent with the first block.
std::pair<float, float> runTone(juce::AudioProcessor& processor, int blocks, float hertz = 300, juce::MidiBuffer first = {})
{
    juce::AudioBuffer<float> audio(2, 480);
    float peak = 0, stray = 0;
    static int clock = 0;
    for (int block = 0; block < blocks; ++block)
    {
        std::array<float, 480> input {};
        for (int i = 0; i < 480; ++i)
            input[static_cast<size_t>(i)] = .25f * std::sin(juce::MathConstants<float>::twoPi * hertz
                                                            * static_cast<float>(clock++) / 48000.f);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 480; ++i)
                audio.setSample(c, i, input[static_cast<size_t>(i)]);
        auto midi = block == 0 ? first : juce::MidiBuffer {};
        processor.processBlock(audio, midi);
        peak = audio.getMagnitude(0, 0, 480);
        stray = 0;
        for (int i = 0; i < 480; ++i)
            stray = std::max(stray, std::abs(audio.getSample(0, i) - input[static_cast<size_t>(i)]));
    }
    return { peak, stray };
}

// Tape Stop winds the whole output down to silence; starting again spins it back up and lands on the live signal
// with nothing left behind. A held MIDI note stops it too.
void checkTapeTransport()
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    set(*processor, "tape_active", 0);
    set(*processor, "transport_stop_time", 300);
    set(*processor, "transport_start_time", 200);
    processor->prepareToPlay(48000, 480);
    check(runTone(*processor, 20).second < 1e-4f, "With the tape running the output must pass straight through");
    set(*processor, "transport_stop", 1);
    check(runTone(*processor, 60).first < .002f, "Tape Stop did not wind the output down to silence");
    set(*processor, "transport_stop", 0);
    check(runTone(*processor, 60).second < 1e-4f, "After starting again the tape did not return to the live signal");

    juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 36, .8f), 0);
    check(runTone(*processor, 60, 300, note).first < .002f, "A held MIDI note did not stop the tape");
    juce::MidiBuffer release;
    release.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
    check(runTone(*processor, 60, 300, release).second < 1e-4f, "Releasing the note did not start the tape again");
    processor->releaseResources();
}

// One generation leaves the sound alone; ten generations lose most of the top end.
void checkGenerations()
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    set(*processor, "tape_active", 0);
    processor->prepareToPlay(48000, 480);
    check(runTone(*processor, 20, 9000).second < 1e-4f, "One generation changed the sound");
    set(*processor, "global_generations", 10);
    const auto worn = runTone(*processor, 80, 9000).first;
    const auto body = runTone(*processor, 80, 300).first;
    check(worn < .25f * body, "Ten generations did not dull the highs");
    check(body > .12f, "Ten generations lost the body of the sound");
    processor->releaseResources();
}

// A cable dragged from the LFO 1 jack onto the Tape Depth knob patches LFO 1's first route to it at +50%.
void checkPatchCables(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before patching");
    drag(editor, "tape_cartridge", centre(editor, "add_slot"));
    dispatch();
    auto& cables = control<juce::Component>(editor, "patch_cables");
    auto& knob = control<juce::Slider>(editor, "tape_depth");
    const auto knobCentre = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
    const auto from = cables.getLocalPoint(&editor, centre(editor, "lfo_1_jack")).toFloat();
    const auto to = cables.getLocalPoint(&editor, knobCentre).toFloat();
    dragWithin(cables, from, to);
    check(value(processor, "lfo_1_target_1") == 1 && std::abs(value(processor, "lfo_1_depth_1") - 50) < .01f,
          "Dragging a cable from LFO 1 onto Tape Depth did not patch it");
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "macro_jack")).toFloat(), to.translated(600, 0));
    check(value(processor, "macro_target_1") == 0, "Dropping a cable away from any knob still patched it");
    // A second cable from the Macro, to show both in the preview.
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "macro_jack")).toFloat(),
               cables.getLocalPoint(&editor, editor.getLocalPoint(&control<juce::Slider>(editor, "tape_wow_rate"),
                   juce::Point<int>(control<juce::Slider>(editor, "tape_wow_rate").getWidth() / 2,
                                    (control<juce::Slider>(editor, "tape_wow_rate").getHeight() - 28) / 2))).toFloat());
    check(value(processor, "macro_target_1") == 2, "Dragging a cable from the Macro onto Wow Rate did not patch it");
    dispatch();
    if (argc >= 16)
        snapshot(editor, argv[15]);
}

// Where a patched knob's plug rests, in the cable layer's coordinates: against the knob's upper right edge.
juce::Point<float> plugOf(juce::Component& editor, juce::Component& cables, const char* knobId)
{
    auto& knob = control<juce::Slider>(editor, knobId);
    const auto room = juce::jmin(knob.getWidth(), knob.getHeight() - 28);
    const auto diameter = static_cast<float>(juce::jmin(room, 47));
    const auto centre = juce::Point<float>(static_cast<float>(knob.getWidth()) * .5f, static_cast<float>(knob.getHeight() - 28) * .5f);
    const auto tip = centre + juce::Point<float>(.94f, -.34f) * (diameter * .5f + 8.f);
    return cables.getLocalPoint(&knob, tip);
}

// Every knob on every card takes a patch cable: each cartridge in turn goes into an empty rack, a cable from
// LFO 1 is dropped on each of its knobs, and the route must point at that knob's own parameter.
void checkEveryKnobPatches()
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before patching every knob");
    const std::vector<std::pair<const char*, std::vector<const char*>>> cards {
        { "tape", { "depth", "wow_rate", "flutter_rate", "flutter_amount" } },
        { "tremolo", { "depth", "rate", "shape" } },
        { "saturation", { "drive", "frequency", "tone", "mix" } },
        { "resampler", { "rate", "drive", "jitter" } },
        { "chorus", { "depth", "rate", "width", "mix" } },
        { "delay", { "feedback", "saturation", "analog", "mix" } },
        { "reverse", { "forward", "pitch", "highpass", "mix" } },
        { "reverb", { "size", "decay", "pre_delay", "mix" } },
        { "stretch", { "ratio", "cycle", "mix" } },
        { "vhs", { "wear", "tracking", "wobble" } },
        { "grain", { "size", "density", "pitch", "mix" } }
    };
    auto& cables = control<juce::Component>(editor, "patch_cables");
    auto* targets = dynamic_cast<juce::AudioParameterChoice*>(&parameter(processor, "lfo_1_target_1"));
    check(targets != nullptr, "LFO 1 has no target list");
    for (const auto& [card, knobs] : cards)
    {
        drag(editor, (juce::String(card) + "_cartridge").toRawUTF8(), centre(editor, "add_slot"));
        dispatch();
        for (const auto* name : knobs)
        {
            const auto id = juce::String(card) + "_" + name;
            auto& knob = control<juce::Slider>(editor, id);
            const auto point = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
            dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "lfo_1_jack")).toFloat(),
                       cables.getLocalPoint(&editor, point).toFloat());
            const auto chosen = juce::roundToInt(value(processor, "lfo_1_target_1"));
            const auto expected = parameter(processor, id).getName(100);
            check(chosen > 0, ("The " + id + " knob would not take a patch cable").toRawUTF8());
            check(targets->choices[chosen].equalsIgnoreCase(expected),
                  ("A cable on " + id + " patched " + targets->choices[chosen] + " instead").toRawUTF8());
            set(processor, "lfo_1_target_1", 0);
        }
        click(editor, "eject_0");
        dispatch();
    }
}

// The envelope follower and the sequencer really move what they are patched to. Both drive Saturation's Drive,
// which leaves the sound untouched at zero: with a loud tone the envelope pushes it up and the sound changes; the
// sequencer alternates full and empty steps, so some blocks come out driven and some clean.
void checkEnvelopeAndSequencer()
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    set(*processor, "tape_active", 0);
    set(*processor, "saturation_active", 1);
    set(*processor, "saturation_drive", 0);
    processor->prepareToPlay(48000, 480);
    check(runTone(*processor, 20).second < 1e-4f, "Saturation at zero drive changed the sound");
    set(*processor, "env_target_1", 5);
    set(*processor, "env_depth_1", 100);
    set(*processor, "env_gain", 12);
    check(runTone(*processor, 20).second > .01f, "The envelope did not drive the patched knob");
    set(*processor, "env_target_1", 0);
    check(runTone(*processor, 20).second < 1e-4f, "Pulling the envelope cable left the knob modulated");

    set(*processor, "seq_target_1", 5);
    set(*processor, "seq_depth_1", 100);
    set(*processor, "seq_glide", 0);
    for (int i = 1; i <= 16; ++i)
        set(*processor, "seq_step_" + juce::String(i), i % 2 == 1 ? 100.f : 0.f);
    int driven = 0, clean = 0;
    for (int block = 0; block < 60; ++block)
    {
        const auto stray = runTone(*processor, 1).second;
        driven += stray > .01f ? 1 : 0;
        clean += stray < 1e-3f ? 1 : 0;
    }
    check(driven > 5 && clean > 5, "The sequencer did not alternate between its steps");
    processor->releaseResources();
}

// Clicking a jack slides out the MOD drawer on that source's tab, clicking it again closes it; drawing in the step
// editor writes the steps; and a cable from the ENV jack patches like any other.
void checkModDrawer(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before the drawer check");
    drag(editor, "chorus_cartridge", centre(editor, "add_slot"));
    dispatch();
    auto& cables = control<juce::Component>(editor, "patch_cables");
    auto& drawer = control<juce::Component>(editor, "mod_drawer");
    const auto tap = [&](const char* jack)
    {
        const auto at = cables.getLocalPoint(&editor, centre(editor, jack)).toFloat();
        dragWithin(cables, at, at);
    };
    check(!drawer.isVisible(), "The MOD drawer was open before any jack was clicked");
    tap("seq_jack");
    check(drawer.isVisible() && find(editor, "seq_steps") != nullptr && control<juce::Component>(editor, "seq_steps").isVisible(),
          "Clicking the SEQ jack did not open the drawer on the sequencer");
    check(editor.getWidth() == 1160 && editor.getHeight() > 688, "The window did not grow downwards for the drawer");
    auto& steps = control<juce::Component>(editor, "seq_steps");
    const auto column = steps.getWidth() / 16.f;
    dragWithin(steps, { 8.f + column * 2.5f, 12.f }, { 8.f + column * 2.5f, 12.f });
    check(value(processor, "seq_step_3") > 80, "Drawing at the top of step 3 did not raise it");
    if (argc >= 18)
        snapshot(editor, argv[17]);
    tap("lfo_1_jack");
    if (argc >= 19)
        snapshot(editor, argv[18]);
    // The X jack opens the XY page: dragging on its pad moves both macros, and a cable from the Y jack takes the
    // first free route.
    tap("xy_x_jack");
    check(drawer.isVisible() && control<juce::Component>(editor, "xy_pad").isVisible()
              && control<juce::Component>(editor, "xy_x_amount").isVisible() && !steps.isVisible(),
          "Clicking the X jack did not open the drawer on the XY pad");
    auto& pad = control<juce::Component>(editor, "xy_pad");
    dragWithin(pad, { 14.f, 14.f }, { 40.f, 30.f });
    check(value(processor, "xy_x_amount") < 40 && value(processor, "xy_y_amount") > 60, "The drawer's XY pad did not move the macros");
    {
        auto& depth = control<juce::Slider>(editor, "chorus_depth");
        const auto spot = editor.getLocalPoint(&depth, juce::Point<int>(depth.getWidth() / 2, (depth.getHeight() - 28) / 2));
        dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "xy_y_jack")).toFloat(), cables.getLocalPoint(&editor, spot).toFloat());
        check(value(processor, "xy_y_target_1") == 9 && value(processor, "xy_y_active_1") == 1,
              "A cable from the Y jack did not patch Chorus Depth");
    }
    if (argc >= 22)
        snapshot(editor, argv[21]);
    set(processor, "xy_y_target_1", 0);
    set(processor, "xy_x_amount", 50);
    set(processor, "xy_y_amount", 50);
    tap("env_jack");
    check(drawer.isVisible() && control<juce::Component>(editor, "env_attack").isVisible() && !steps.isVisible(),
          "Clicking the ENV jack did not switch the drawer to the envelope");
    // The jack's name above the socket is part of its tile too.
    const auto label = cables.getLocalPoint(&editor, centre(editor, "env_jack")).toFloat().translated(0, -22);
    dragWithin(cables, label, label);
    check(!drawer.isVisible(), "Clicking the open source's tile again did not close the drawer");
    check(editor.getHeight() == 688 && editor.getWidth() == 1160, "The window did not return to its size after the drawer closed");

    auto& knob = control<juce::Slider>(editor, "chorus_depth");
    const auto point = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "env_jack")).toFloat(), cables.getLocalPoint(&editor, point).toFloat());
    check(value(processor, "env_target_1") == 9, "A cable from the ENV jack did not patch Chorus Depth");
}

// Patching works for every card in a full rack too, not only when a card is alone: VHS first, then Resampler and
// Chorus, as people often build it, with a cable already running to another card.
void checkMixedRackPatching(bool onScreen)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    // On screen the editor has a window: frames run, the drawers drop in and the cables swing, as in the app.
    if (onScreen)
    {
        editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor.setVisible(true);
    }
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before the mixed rack check");
    for (const auto* id : { "vhs_cartridge", "resampler_cartridge", "chorus_cartridge" })
    {
        drag(editor, id, centre(editor, "add_slot"));
        for (int i = 0; i < (onScreen ? 12 : 1); ++i)
            dispatch();
    }
    auto& cables = control<juce::Component>(editor, "patch_cables");
    const auto patch = [&](const char* jack, const char* knobId)
    {
        auto& knob = control<juce::Slider>(editor, knobId);
        const auto point = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
        dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, jack)).toFloat(), cables.getLocalPoint(&editor, point).toFloat());
        (void) editor.createComponentSnapshot(editor.getLocalBounds());
        for (int i = 0; i < (onScreen ? 4 : 0); ++i)
            dispatch();
    };
    patch("lfo_1_jack", "chorus_depth");
    check(value(processor, "lfo_1_target_1") == 9, "Could not patch Chorus Depth in a mixed rack");
    for (const auto& [knob, target] : { std::pair { "vhs_wear", 23 }, std::pair { "vhs_tracking", 24 }, std::pair { "vhs_wobble", 25 } })
    {
        patch("lfo_2_jack", knob);
        check(value(processor, "lfo_2_target_1") == target,
              (juce::String("Could not patch ") + knob + " in a mixed rack (got " + juce::String(value(processor, "lfo_2_target_1")) + ")").toRawUTF8());
        set(processor, "lfo_2_target_1", 0);
    }
}

// With two copies of an effect, either copy's knobs take a cable, also when the first copy is scrolled out of view:
// here the second VHS is in view and the first is not.
void checkSecondCopyPatching()
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before the second copy check");
    for (const auto* id : { "vhs_cartridge", "tape_cartridge", "chorus_cartridge", "reverb_cartridge", "saturation_cartridge",
                            "vhs_cartridge" })
    {
        drag(editor, id, centre(editor, "add_slot"));
        dispatch();
    }
    auto& viewport = control<juce::Viewport>(editor, "rack_viewport");
    viewport.setViewPosition(viewport.getViewedComponent()->getWidth(), 0);
    auto& cables = control<juce::Component>(editor, "patch_cables");
    auto& knob = control<juce::Slider>(editor, "cart2_vhs_wear");
    const auto window = editor.getLocalArea(&viewport, viewport.getLocalBounds());
    const auto point = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
    check(window.contains(point) && !window.contains(editor.getLocalPoint(&control<juce::Slider>(editor, "vhs_wear"), juce::Point<int>())),
          "The second VHS should be in view and the first out of it");
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "env_jack")).toFloat(), cables.getLocalPoint(&editor, point).toFloat());
    check(value(processor, "env_target_1") == 23, "The second VHS's Wear knob would not take a cable");
}

// A patched card scrolled out of the rack keeps its route; its plug leaves the reach of the mouse instead of
// floating over whatever covers the place, and returns to the knob when the card scrolls back.
void checkCablesWhileScrolling(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> owner(createPluginFilter());
    auto& processor = *owner;
    const auto opened = openEditor(processor);
    auto& editor = *opened;
    click(editor, "next_preset");
    check(presetName(editor) == "Initialize", "Could not reset the rack before the scrolling cable check");
    for (const auto* id : { "delay_cartridge", "tape_cartridge", "chorus_cartridge", "reverb_cartridge", "saturation_cartridge" })
    {
        drag(editor, id, centre(editor, "add_slot"));
        dispatch();
    }
    auto& viewport = control<juce::Viewport>(editor, "rack_viewport");
    viewport.setViewPosition(0, 0);
    auto& cables = control<juce::Component>(editor, "patch_cables");
    auto& knob = control<juce::Slider>(editor, "delay_mix");
    const auto knobPoint = editor.getLocalPoint(&knob, juce::Point<int>(knob.getWidth() / 2, (knob.getHeight() - 28) / 2));
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "lfo_1_jack")).toFloat(),
               cables.getLocalPoint(&editor, knobPoint).toFloat());
    check(value(processor, "lfo_1_target_1") == 14, "Could not patch LFO 1 to the Delay Mix knob");
    const auto paintNow = [&editor] { (void) editor.createComponentSnapshot(editor.getLocalBounds()); };

    paintNow();
    const auto plug = plugOf(editor, cables, "delay_mix");
    check(cables.hitTest(juce::roundToInt(plug.x), juce::roundToInt(plug.y)), "The plug on a visible knob cannot be grabbed");

    viewport.setViewPosition(viewport.getViewedComponent()->getWidth(), 0);
    paintNow();
    const auto window = cables.getLocalArea(&editor, editor.getLocalArea(&viewport, viewport.getLocalBounds())).toFloat();
    const auto hidden = plugOf(editor, cables, "delay_mix");
    check(!window.contains(hidden), "The Delay card did not scroll out of view");
    check(value(processor, "lfo_1_target_1") == 14, "Scrolling a patched card away dropped its route");
    check(!cables.hitTest(juce::roundToInt(plug.x), juce::roundToInt(plug.y)),
          "A plug stayed grabbable where its card used to be");
    // Dropping a cable on a knob that is out of view must not patch it.
    dragWithin(cables, cables.getLocalPoint(&editor, centre(editor, "lfo_2_jack")).toFloat(), hidden);
    check(value(processor, "lfo_2_target_1") == 0, "A cable patched a knob that was scrolled out of view");
    if (argc >= 17)
        snapshot(editor, argv[16]);

    viewport.setViewPosition(0, 0);
    paintNow();
    const auto back = plugOf(editor, cables, "delay_mix");
    check(back.getDistanceFrom(plug) < 1.f && cables.hitTest(juce::roundToInt(back.x), juce::roundToInt(back.y)),
          "The plug did not return to its knob when the card scrolled back");
}

// A rack saved before the VHS cartridge existed numbered second copies from 9; it must come back with the same
// cartridges in the same order.
void checkRackMigration()
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    auto block = save(*processor);
    auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), static_cast<int>(block.getSize()));
    check(xml != nullptr, "Could not read the saved state");
    // Old numbering: Delay (5), second Tape (9), second Delay (14). Packed as value + 2, five bits a slot.
    const juce::uint64 packed = (juce::uint64 { 1 } << 63) | (juce::uint64 { 5 + 2 } << 0) | (juce::uint64 { 9 + 2 } << 5)
                              | (juce::uint64 { 14 + 2 } << 10);
    xml->setAttribute("stateVersion", 5);
    xml->setAttribute("cartridgeOrder", juce::String::toHexString(static_cast<juce::int64>(packed)));
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary(*xml, old);
    restore(*processor, old);
    auto editor = openEditor(*processor);
    check(find(*editor, "delay_power") != nullptr && find(*editor, "cart2_tape_power") != nullptr
              && find(*editor, "cart2_delay_power") != nullptr && find(*editor, "vhs_power") == nullptr,
          "An old rack did not come back with the same cartridges");
    check(centre(*editor, "delay_power").x < centre(*editor, "cart2_tape_power").x
              && centre(*editor, "cart2_tape_power").x < centre(*editor, "cart2_delay_power").x,
          "An old rack did not keep its order");

    // A rack saved before the Grain cartridge numbered second copies from 10: VHS (9), second Tape (10) and
    // second VHS (19).
    const juce::uint64 vhsEra = (juce::uint64 { 1 } << 63) | (juce::uint64 { 9 + 2 } << 0) | (juce::uint64 { 10 + 2 } << 5)
                              | (juce::uint64 { 19 + 2 } << 10);
    xml->setAttribute("stateVersion", 6);
    xml->setAttribute("cartridgeOrder", juce::String::toHexString(static_cast<juce::int64>(vhsEra)));
    editor.reset();
    juce::MemoryBlock saved;
    juce::AudioProcessor::copyXmlToBinary(*xml, saved);
    restore(*processor, saved);
    const auto reopened = openEditor(*processor);
    check(find(*reopened, "vhs_power") != nullptr && find(*reopened, "cart2_tape_power") != nullptr
              && find(*reopened, "cart2_vhs_power") != nullptr && find(*reopened, "grain_power") == nullptr,
          "A rack from before the Grain cartridge did not come back with the same cartridges");
    check(centre(*reopened, "vhs_power").x < centre(*reopened, "cart2_tape_power").x
              && centre(*reopened, "cart2_tape_power").x < centre(*reopened, "cart2_vhs_power").x,
          "A rack from before the Grain cartridge did not keep its order");
}

// The Stretch cartridge seats like any other and shows its four featured controls and engine selector.
void checkStretchCard(int argc, char** argv)
{
    const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
    const auto editor = openEditor(*processor);
    drag(*editor, "stretch_cartridge", centre(*editor, "add_slot"));
    dispatch();
    for (const auto* id : { "stretch_ratio", "stretch_cycle", "stretch_mix", "stretch_quick_mode" })
        check(find(*editor, id) != nullptr, (juce::String("The Stretch card is missing ") + id).toRawUTF8());
    check(value(*processor, "stretch_active") == 1, "Inserting the Stretch cartridge did not power it");
    check(find(*editor, "stretch_map_screen") != nullptr, "The Stretch card is missing its map screen");

    // Dragging the bars on the card screen moves LAND and START, snapped to sixteenths of the program.
    auto& screen = control<juce::Component>(*editor, "stretch_map_screen");
    const auto graph = screen.getLocalBounds().toFloat().reduced(.5f).reduced(7, 5).withTrimmedTop(13);
    const auto at = [&graph](float position) { return juce::Point<float>(graph.getX() + graph.getWidth() * position,
                                                                         graph.getCentreY()); };
    dragWithin(screen, at(.75f), at(.5f));
    check(std::abs(value(*processor, "stretch_land") - 50) < .01f, "Dragging the LAND bar did not move Land");
    dragWithin(screen, at(0), at(.27f));
    check(std::abs(value(*processor, "stretch_start") - 25) < .01f, "Dragging the START bar did not snap Start");
    dragWithin(screen, at(.25f), at(0));
    check(value(*processor, "stretch_start") < .01f, "The START bar could not be dragged back");

    // Run a little audio so the map shows a playhead part way through the stretch.
    set(*processor, "stretch_ratio", 4);
    set(*processor, "stretch_shape", 3);
    processor->prepareToPlay(48000, 480);
    juce::AudioBuffer<float> audio(2, 480);
    juce::MidiBuffer midi;
    for (int block = 0; block < 60; ++block)
    {
        for (int i = 0; i < 480; ++i)
            audio.setSample(0, i, .3f * std::sin(.06f * static_cast<float>(block * 480 + i)));
        audio.copyFrom(1, 0, audio, 0, 0, 480);
        processor->processBlock(audio, midi);
    }
    for (int i = 0; i < 4; ++i)
        dispatch();
    if (argc >= 11)
        snapshot(*editor, argv[10]);
    if (argc >= 13)
    {
        // The power-up arcs, drawn half way round and at full strength over the seated Stretch card.
        auto image = editor->createComponentSnapshot(editor->getLocalBounds());
        {
            juce::Graphics g(image);
            auto& card = control<juce::Component>(*editor, "stretch_map_screen");
            const auto* panel = card.getParentComponent();
            const auto area = editor->getLocalArea(panel, panel->getLocalBounds()).toFloat().reduced(1.5f);
            rack::retro::electricFrame(g, area.translated(-300, 0), .6f, 1.f, 3);
            rack::retro::electricBloom(g, area, 1.f);
            g.drawImage(editor->createComponentSnapshot(area.toNearestInt()), area);
            rack::retro::electricFrame(g, area, 1.f, 1.f, 7);
        }
        juce::File file(juce::File::getCurrentWorkingDirectory().getChildFile(argv[12]));
        file.deleteFile();
        juce::FileOutputStream stream(file);
        juce::PNGImageFormat().writeImageToStream(image, stream);
    }

    if (argc >= 14)
    {
        // A frame from the knobs' drop: held part way up, drawn larger, with their shadows thrown wide.
        for (const auto* id : { "stretch_ratio", "stretch_cycle", "stretch_mix" })
            control<juce::Component>(*editor, id).getProperties().set("cassette_drop", .6f);
        snapshot(*editor, argv[13]);
        for (const auto* id : { "stretch_ratio", "stretch_cycle", "stretch_mix" })
            control<juce::Component>(*editor, id).getProperties().remove("cassette_drop");
    }

    click(*editor, "stretch_controls");
    for (int i = 0; i < 30; ++i)
        dispatch();
    check(find(*editor, "stretch_map") != nullptr, "The Stretch controls panel is missing its map");
    if (argc >= 12)
        snapshot(*editor, argv[11]);
    processor->releaseResources();
}

int main(int argc, char** argv)
{
    try
    {
        juce::ScopedJuceInitialiser_GUI gui;
        checkCardLayouts();
        checkRackMigration();
        checkTapeTransport();
        checkGenerations();
        checkPatchCables(argc, argv);
        checkCablesWhileScrolling(argc, argv);
        checkEveryKnobPatches();
        checkMixedRackPatching(false);
        checkMixedRackPatching(true);
        checkSecondCopyPatching();
        checkEnvelopeAndSequencer();
        checkModDrawer(argc, argv);
        checkVhsCard(argc, argv);
        checkGrainCard(argc, argv);
        checkStretchCard(argc, argv);
        checkUniformScaling(argc, argv);
        const std::unique_ptr<juce::AudioProcessor> processor(createPluginFilter());
        const auto editor = openEditor(*processor);
        const auto designSize = editor->getLocalBounds().getBottomRight();
        check(value(*processor, "tape_active") == 1, "Existing default parameter changed");
        checkFactory(*editor, *processor);

        // The master section is always on the deck and drives the global parameters.
        for (const auto* id : { "input_gain", "output_gain", "global_width", "global_pitch", "global_detune", "global_mix" })
        {
            auto& knob = control<juce::Slider>(*editor, juce::String("master_") + id);
            check(editor->getLocalBounds().contains(editor->getLocalArea(&knob, knob.getLocalBounds())),
                  "A master control is clipped by the window");
        }
        control<juce::Slider>(*editor, "master_input_gain").setValue(-6, juce::sendNotificationSync);
        control<juce::Slider>(*editor, "master_global_mix").setValue(40, juce::sendNotificationSync);
        check(std::abs(value(*processor, "input_gain") + 6) < .01f && std::abs(value(*processor, "global_mix") - 40) < .01f,
              "The master knobs did not set their parameters");
        set(*processor, "output_gain", 3);
        dispatch();
        check(std::abs(control<juce::Slider>(*editor, "master_output_gain").getValue() - 3) < .01f,
              "The master Out knob missed a parameter change");
        control<juce::Slider>(*editor, "master_input_gain").setValue(0, juce::sendNotificationSync);
        control<juce::Slider>(*editor, "master_global_mix").setValue(100, juce::sendNotificationSync);
        set(*processor, "output_gain", 0);
        dispatch();

        drag(*editor, "delay_cartridge", centre(*editor, "add_slot"));
        dispatch();
        check(find(*editor, "delay_feedback") != nullptr, "Drop into the empty rack failed");
        click(*editor, "add_slot");
        check(find(*editor, "slot_1") != nullptr && find(*editor, "cart2_delay_feedback") == nullptr,
              "Plus did not add an empty slot");
        drag(*editor, "delay_cartridge", centre(*editor, "slot_1"));
        dispatch();
        check(find(*editor, "cart2_delay_feedback") != nullptr, "Could not fill the empty slot with another Delay");
        click(*editor, "add_slot");

        auto& first = control<juce::Slider>(*editor, "delay_feedback");
        auto& second = control<juce::Slider>(*editor, "cart2_delay_feedback");
        check(first.getMaximum() == 100 && second.getMaximum() == 100, "Feedback exceeds 100% in the UI");
        first.setValue(27, juce::sendNotificationSync);
        second.showTextBox();
        auto& numericDisplay = control<juce::Label>(second, "cart2_delay_feedback_value");
        auto* numericEditor = numericDisplay.getCurrentTextEditor();
        check(numericEditor != nullptr, "The skinned value display did not open for typing");
        numericEditor->setText("42", false);
        second.hideTextBox(false);
        checkReadout(*editor, "delay_feedback");
        checkReadout(*editor, "cart2_delay_feedback");
        checkPlus(*editor);
        checkShelf(*editor);
        check(std::abs(value(*processor, "delay_feedback") - 27) < .01f
                 && std::abs(value(*processor, "cart2_delay_feedback") - 42) < .01f,
              "Delay knobs did not control separate instances");
        set(*processor, "delay_note", 2);
        set(*processor, "delay_timing", 2);
        set(*processor, "cart2_delay_note", 4);
        dispatch();
        check(control<juce::TextButton>(*editor, "delay_division").getButtonText() == "1/16 Dotted"
                 && control<juce::TextButton>(*editor, "cart2_delay_division").getButtonText() == "1/4",
              "Divisions did not update independently");
        if (argc >= 2)
            snapshot(*editor, argv[1]);
        if (argc >= 5)
        {
            editor->setSize(1560, 900);
            checkShelf(*editor);
            snapshot(*editor, argv[4]);
            editor->setSize(1160, 690);
        }

        const auto beforeDelaySize = editor->getLocalBounds().getBottomRight();
        editor->setSize(juce::roundToInt(designSize.x * 1.25f), juce::roundToInt(designSize.y * 1.25f));
        click(*editor, "cart2_delay_controls");
        auto& delayInspector = control<juce::Component>(*editor, "effects_inspector");
        check(find(delayInspector, "cart2_delay_time") != nullptr, "Full secondary Delay controls are missing");
        auto& envelope = control<juce::Component>(delayInspector, "echo_envelope");
        auto& delayClose = control<juce::TextButton>(delayInspector, "close_controls");
        auto& lateDelayControl = control<juce::Slider>(delayInspector, "cart2_delay_mix");
        auto* delayViewport = lateDelayControl.findParentComponentOfClass<juce::Viewport>();
        check(delayViewport != nullptr && delayViewport->getViewedComponent() != nullptr,
              "Delay controls are not hosted in a viewport");
        const auto lateDelayBounds = delayViewport->getViewedComponent()->getLocalArea(
            &lateDelayControl, lateDelayControl.getLocalBounds());
        const auto envelopeBeforeScroll = editor->getLocalArea(&envelope, envelope.getLocalBounds());
        const auto closeBeforeScroll = editor->getLocalArea(&delayClose, delayClose.getLocalBounds());
        check(!delayViewport->getViewArea().contains(lateDelayBounds), "Long Delay panel should require scrolling");
        delayViewport->setViewPosition(0, lateDelayBounds.getY());
        check(delayViewport->getViewPositionY() > 0 && delayViewport->getViewArea().contains(lateDelayBounds),
              "Could not scroll the last Delay control into view at 125% zoom");
        check(editor->getLocalArea(&envelope, envelope.getLocalBounds()) == envelopeBeforeScroll
                  && editor->getLocalArea(&delayClose, delayClose.getLocalBounds()) == closeBeforeScroll,
              "The Delay envelope or close button moved while scrolling the controls");
        click(delayInspector, "close_controls");
        dispatch();
        editor->setSize(beforeDelaySize.x, beforeDelaySize.y);

        const auto oldFirst = centre(*editor, "delay_power").x, oldSecond = centre(*editor, "cart2_delay_power").x;
        check(oldFirst < oldSecond, "Unexpected initial Delay order");
        drag(*editor, "slot_1", centre(*editor, "slot_0"));
        dispatch();
        check(centre(*editor, "delay_power").x > centre(*editor, "cart2_delay_power").x,
              "Drag did not change processing order");
        drag(*editor, "slot_1", { 60, 16 });
        dispatch();
        check(centre(*editor, "delay_power").x > centre(*editor, "cart2_delay_power").x,
              "Dropping outside the rack changed the order");
        {
            const std::unique_ptr<juce::AudioProcessor> resumed(createPluginFilter());
            restore(*resumed, save(*processor));
            const auto reopened = openEditor(*resumed);
            check(centre(*reopened, "delay_power").x > centre(*reopened, "cart2_delay_power").x
                     && find(*reopened, "slot_2") != nullptr
                     && std::abs(value(*resumed, "cart2_delay_feedback") - 42) < .01f,
                  "Session did not restore order, empty slots and duplicate values");
        }

        for (const auto& prefix : { juce::String("delay_"), juce::String("cart2_delay_") })
        {
            set(*processor, prefix + "sync", 0);
            set(*processor, prefix + "feedback", 0);
            set(*processor, prefix + "mix", 50);
            set(*processor, prefix + "saturation", 0);
            set(*processor, prefix + "analog", 0);
        }
        set(*processor, "delay_time", 80);
        set(*processor, "cart2_delay_time", 160);
        const auto combined = impulse(*processor);
        check(echo(combined, 80) > .01f && echo(combined, 160) > .01f && echo(combined, 240) > .01f,
              "The two Delays did not each contribute to the serial DSP");
        click(*editor, "cart2_delay_power");
        dispatch();
        check(find(*editor, "cart2_delay_feedback") != nullptr && value(*processor, "cart2_delay_active") == 0,
              "Bypass removed the cartridge or did not update its own power");
        const auto bypassed = impulse(*processor);
        check(echo(bypassed, 80) > .01f && echo(bypassed, 160) < .001f && echo(bypassed, 240) < .001f,
              "Secondary bypass changed the wrong Delay");
        click(*editor, "eject_0");
        dispatch();
        check(find(*editor, "cart2_delay_feedback") == nullptr && find(*editor, "delay_feedback") != nullptr,
              "Eject removed the wrong instance");

        auto& add = control<juce::Component>(*editor, "add_slot");
        const auto lowerEdge = editor->getLocalPoint(&add, juce::Point<int>(add.getWidth() / 2, add.getHeight() - 21));
        drag(*editor, "reverb_cartridge", lowerEdge);
        dispatch();
        check(find(*editor, "reverb_decay") != nullptr, "Dropping directly on plus did not insert a filled slot");
        click(*editor, "generate");
        const auto generated = presetName(*editor);
        check(generated.containsChar(' ') && generated != "Custom" && !generated.startsWith("Generated"),
              "Generate did not display its named variation");
        // A generated rack mixes three or four cartridges, set up in different presets.
        for (uint32_t seed = 1; seed < 40; ++seed)
        {
            int active = 0;
            for (const auto& entry : rack::generatePreset(0, seed).values)
                if (juce::String(entry.id).endsWith("_active") && entry.value > .5f)
                    ++active;
            check(active >= 3 && active <= 4, "Generate did not mix three or four cartridges");
        }
        {
            const std::unique_ptr<juce::AudioProcessor> resumed(createPluginFilter());
            restore(*resumed, save(*processor));
            const auto reopened = openEditor(*resumed);
            check(presetName(*reopened) == generated, "Generated variation did not survive reload");
        }

        if (argc >= 3)
        {
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
            const auto state = rack::readPresetComponent(file);
            check(state.has_value(), "Could not read the converted legacy VST3 preset");
            check(!rack::readPresetComponent(file.getSiblingFile("import-report.json")), "Accepted a non-VST3 preset");
            restore(*processor, *state);
            dispatch();
            check(value(*processor, "reverse_active") == 1 && value(*processor, "reverse_pitch") == -12,
                  "Legacy imported preset lost data");
            check(find(*editor, "reverse_pitch") != nullptr, "Legacy settings did not populate the native rack");
        }

        click(*editor, "next_preset");
        check(presetName(*editor) == "Initialize", "Could not start the Chorus editor check");
        click(*editor, "chorus_cartridge");
        set(*processor, "chorus_depth", 11.7f);
        set(*processor, "chorus_rate", 1.33f);
        set(*processor, "chorus_saturation", 7.27f);
        set(*processor, "chorus_width", 30);
        set(*processor, "chorus_mix", 30);
        click(*editor, "chorus_controls");
        auto& chorusInspector = control<juce::Component>(*editor, "effects_inspector");
        auto& chorusClose = control<juce::TextButton>(chorusInspector, "close_controls");
        check(chorusInspector.getTitle() == "Chorus"
                  && chorusInspector.getLocalBounds().contains(
                      chorusInspector.getLocalArea(&chorusClose, chorusClose.getLocalBounds())),
              "The Chorus inspector title or close button is missing");
        auto& stereo = control<juce::TextButton>(chorusInspector, "chorus_stereo");
        check(stereo.getWidth() >= 96 && stereo.getHeight() >= 26, "Expanded Chorus stereo switch is too small");
        const auto wasStereo = stereo.getToggleState();
        stereo.setToggleState(!wasStereo, juce::sendNotificationSync);
        check(value(*processor, "chorus_stereo") == (wasStereo ? 0.0f : 1.0f),
              "The expanded Chorus stereo switch did not update the parameter");
        set(*processor, "chorus_stereo", wasStereo ? 1.0f : 0.0f);
        check(stereo.getToggleState() == wasStereo, "The expanded Chorus stereo switch missed a parameter change");
        auto& waveform = control<juce::ComboBox>(chorusInspector, "chorus_waveform");
        waveform.setSelectedId(2, juce::sendNotificationSync);
        check(waveform.getText() == "Sine" && value(*processor, "chorus_waveform") == 1,
              "The skinned selector did not update the parameter");
        waveform.setSelectedId(1, juce::sendNotificationSync);
        for (const auto size : { juce::Point<int>(900, 650), juce::Point<int>(1160, 690), juce::Point<int>(1560, 900) })
        {
            editor->setSize(size.x, size.y);
            checkReadout(chorusInspector, "chorus_saturation");
            checkPlus(*editor);
            checkShelf(*editor);
            check(control<juce::Component>(*editor, "presets").getWidth() > 160,
                  "Preset display is too narrow to show preset names");
        }
        if (argc >= 4)
        {
            for (int i = 0; i < 4; ++i)
                dispatch();
            snapshot(*editor, argv[3]);
        }

        std::cout << "Native rack, preset arrows, two independent Delay paths, bypass, reorder, session restore and legacy import passed.\n";
        std::cout << "Control assets, numeric readout spacing, selector and full-height plus passed at three sizes.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

