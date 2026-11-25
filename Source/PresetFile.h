#pragma once

#include <juce_core/juce_core.h>
#include <cstring>
#include <optional>

namespace rack
{
inline std::optional<juce::MemoryBlock> readPresetComponent(const juce::File& file)
{
    juce::FileInputStream stream(file);
    const auto size = stream.getTotalLength();
    if (!stream.openedOk() || size < 56 || size > 1024 * 1024)
        return std::nullopt;

    const auto matches = [&stream](const char* fourBytes)
    {
        char text[4];
        return stream.read(text, 4) == 4 && std::memcmp(text, fourBytes, 4) == 0;
    };
    if (!matches("VST3") || stream.readInt() != 1 || !stream.setPosition(40))
        return std::nullopt;

    const auto table = stream.readInt64();
    if (table < 48 || table > size - 8 || !stream.setPosition(table) || !matches("List"))
        return std::nullopt;
    const auto count = stream.readInt();
    if (count < 1 || count > (size - table - 8) / 20)
        return std::nullopt;

    for (int i = 0; i < count; ++i)
    {
        const auto component = matches("Comp");
        const auto offset = stream.readInt64();
        const auto length = stream.readInt64();
        if (!component)
            continue;
        if (offset < 48 || offset >= table || length < 9 || length > table - offset || !stream.setPosition(offset))
            return std::nullopt;
        juce::MemoryBlock data;
        if (stream.readIntoMemoryBlock(data, static_cast<int>(length)) == static_cast<size_t>(length))
            return data;
        return std::nullopt;
    }
    return std::nullopt;
}
}
