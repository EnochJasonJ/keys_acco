#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <optional>

struct MidiEvent
{
    double time = 0;
    int size = 0;
    std::array<std::uint8_t, 8> bytes {};
};

struct MidiTrigger
{
    int status;
    int number;

    bool operator==(const MidiTrigger&) const = default;

    static bool valid(int status, int number)
    {
        const auto type = status & 0xf0;
        return status >= 0 && status <= 0xff
            && (type == 0x90 || type == 0xb0 || type == 0xc0)
            && number >= 0 && number <= 127;
    }

    static std::optional<MidiTrigger> from(const MidiEvent& event)
    {
        const int status = event.bytes[0];
        const int number = event.bytes[1];
        const int type = status & 0xf0;
        if (!valid(status, number) || event.size != (type == 0xc0 ? 2 : 3))
            return {};
        if (type != 0xc0 && event.bytes[2] == 0)
            return {};
        return MidiTrigger { status, number };
    }

    juce::String description() const
    {
        return "Ch " + juce::String((status & 15) + 1)
            + ((status & 0xf0) == 0x90 ? "  Note " : (status & 0xf0) == 0xb0 ? "  CC " : "  Program ")
            + juce::String(number);
    }
};

