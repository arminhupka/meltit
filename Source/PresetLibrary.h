#pragma once

#include <juce_core/juce_core.h>
#include "MeltitPresets.h"

namespace rack
{
// %APPDATA%/Producersources/MELTIT (~/Library/Application Support/Producersources/MELTIT on macOS).
inline juce::File dataDirectory()
{
    auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile("Application Support");
   #endif
    return base.getChildFile("Producersources").getChildFile("MELTIT");
}

// The preset library in the data directory. The presets are built into the plug-in; whenever the folder is
// missing or holds no presets, they are unpacked there. Existing files are never overwritten.
inline juce::File presetLibraryDirectory()
{
    static const auto directory = []
    {
        const auto root = dataDirectory();
        const auto target = root.getChildFile("Presets");
        if (target.findChildFiles(juce::File::findFiles, true, "*.vstpreset").isEmpty())
        {
            juce::MemoryInputStream stream(MeltitPresets::MeltitPresets_zip, MeltitPresets::MeltitPresets_zipSize, false);
            juce::ZipFile zip(stream);
            // Unpack beside the target and move it into place, so an interrupted run (a host closing while
            // the library is written) never leaves a half-filled folder that would pass for a complete one.
            const auto staging = root.getChildFile("Presets.unpacking");
            staging.deleteRecursively();
            if (zip.uncompressTo(staging, true).wasOk() && staging.getChildFile("Presets").isDirectory())
            {
                target.deleteRecursively();
                staging.getChildFile("Presets").moveFileTo(target);
            }
            staging.deleteRecursively();
        }
        else
        {
            // Debris of an earlier unpacking that was cut short.
            const auto leftover = root.getChildFile("Presets.unpacking");
            if (leftover.exists())
                leftover.deleteRecursively();
        }
        target.createDirectory();
        return target;
    }();
    return directory;
}
}
