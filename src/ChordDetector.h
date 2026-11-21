#pragma once

#include <array>
#include <cstdint>

// Nonnegative chord IDs encode quality * 12 + root pitch class.
class ChordDetector
{
public:
    enum Quality { major, minor, dominant7, minor7, major7 };

    int chord() const noexcept { return current; }

    void note(int channel, int number, bool down) noexcept
    {
        const auto bit = static_cast<std::uint16_t>(1u << channel);
        auto& channels = held[static_cast<std::size_t>(number)];
        const bool wasHeld = channels != 0;
        if (down)
            channels |= bit;
        else
            channels &= static_cast<std::uint16_t>(~bit);

        const bool isHeld = channels != 0;
        if (wasHeld == isHeld)
            return;

        auto& count = pitchCounts[static_cast<std::size_t>(number % 12)];
        count += isHeld ? 1 : -1;
        recognise(down);
    }

    void releaseChannel(int channel) noexcept
    {
        const auto bit = static_cast<std::uint16_t>(1u << channel);
        for (std::size_t i = 0; i < held.size(); ++i)
        {
            auto& channels = held[i];
            if ((channels & bit) != 0)
            {
                channels &= static_cast<std::uint16_t>(~bit);
                if (channels == 0)
                    --pitchCounts[i % 12];
            }
        }
        recognise(false);
    }

private:
    void recognise(bool noteOn) noexcept
    {
        unsigned pitches = 0;
        for (std::size_t i = 0; i < pitchCounts.size(); ++i)
            if (pitchCounts[i] != 0)
                pitches |= 1u << i;

        constexpr std::array shapes {
            0x091u, // 0, 4, 7
            0x089u, // 0, 3, 7
            0x491u, // 0, 4, 7, 10
            0x489u, // 0, 3, 7, 10
            0x891u  // 0, 4, 7, 11
        };
        const auto rotate = [](unsigned shape, int root)
        {
            return ((shape << root) | (shape >> (12 - root))) & 0xfffu;
        };

        // Releasing an individual chord tone should not turn a held seventh into a triad.
        if (!noteOn && current >= 0)
        {
            const auto previous = rotate(shapes[static_cast<std::size_t>(current / 12)], current % 12);
            if ((pitches & previous) == pitches)
                return;
        }

        for (std::size_t quality = 0; quality < shapes.size(); ++quality)
            for (int root = 0; root < 12; ++root)
                if (pitches == rotate(shapes[quality], root))
                {
                    current = static_cast<int>(quality) * 12 + root;
                    return;
                }
        // Extra notes and incomplete voicings retain the last detected chord.
    }

    std::array<std::uint16_t, 128> held {};
    std::array<int, 12> pitchCounts {};
    int current = -1;
};
