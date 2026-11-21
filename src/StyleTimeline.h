#pragma once

#include <array>
#include <vector>

enum Section { mainASection, mainBSection, introSection, fillSection, endingSection,
               mainCSection, breakSection, sectionCount };

inline constexpr std::array sectionNames { "Main A", "Main B", "Intro", "Fill", "Ending", "Main C", "Break" };

struct StyleBar
{
    unsigned kick, snare, hat, bass;
    unsigned cutTails = 0;
    bool bassFifthOnBeatThree = true;
};

struct BarRange
{
    int first;
    int last;

    bool operator==(const BarRange&) const = default;
};

struct StyleTimeline
{
    static constexpr int maxBars = 64;
    std::vector<StyleBar> bars;
    std::array<BarRange, sectionCount> assignments;

    static StyleTimeline starter()
    {
        // Preserve the grooves and default one-bar assignments from the live arranger.
        return {
            {
                { 0x11, 0x44, 0xff, 0x11 },                 // Main A
                { 0x29, 0x44, 0xff, 0x55 },                 // Main B
                { 0x41, 0x90, 0xff, 0x11 },                 // Intro
                { 0x21, 0xd4, 0xff, 0x45 },                 // Fill
                { 0x11, 0x14, 0x1f, 0x11, 0, false },       // Ending
                { 0x95, 0x54, 0xff, 0xab },                 // Main C
                { 0x01, 0x01, 0x03, 0x01, 0xfc }            // Break
            },
            { BarRange { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 5 }, { 6, 6 } }
        };
    }
};
