#pragma once

#include <array>
#include <bit>
#include <cstdint>

// Nonnegative chord IDs encode quality * 12 + root pitch class.
class ChordDetector
{
public:
    enum Quality { major, minor, dominant7, minor7, major7, sus2, sus4, diminished, augmented };

    int chord() const noexcept { return current; }
    bool isHeld(int number) const noexcept { return held[static_cast<std::size_t>(number)] != 0; }

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
            0x891u, // 0, 4, 7, 11
            0x085u, // 0, 2, 7
            0x0a1u, // 0, 5, 7
            0x049u, // 0, 3, 6
            0x111u  // 0, 4, 8
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

        if (const auto count = std::popcount(pitches); count < 3 || count > 4)
            return;

        int lowest = 0;
        while (lowest < static_cast<int>(held.size()) && held[static_cast<std::size_t>(lowest)] == 0)
            ++lowest;

        // Sus2 and sus4 can describe the same pitches, and augmented triads
        // are symmetric. Prefer the lowest sounding note as root when it fits.
        int best = -1;
        int bestDistance = 12;
        for (std::size_t quality = 0; quality < shapes.size(); ++quality)
            for (int root = 0; root < 12; ++root)
                if (pitches == rotate(shapes[quality], root))
                {
                    const int distance = (root - lowest % 12 + 12) % 12;
                    if (distance < bestDistance)
                    {
                        best = static_cast<int>(quality) * 12 + root;
                        bestDistance = distance;
                    }
                }
        if (best >= 0)
            current = best;
        // Extra notes and incomplete voicings retain the last detected chord.
    }

    std::array<std::uint16_t, 128> held {};
    std::array<int, 12> pitchCounts {};
    int current = -1;
};
