#include "Arranger.h"
#include <algorithm>
#include <cmath>

namespace
{
constexpr int stepsPerBar = 8;

float triangle(float& phase, float advance) noexcept
{
    phase += advance;
    if (phase >= 1.0f)
        phase -= 1.0f;
    return 1.0f - 4.0f * std::abs(phase - 0.5f);
}

float envelope(float& level, float decay) noexcept
{
    const auto result = level;
    level *= decay;
    if (level < 0.0001f)
        level = 0;
    return result;
}
}

void Arranger::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    const auto decay = [&](double seconds) { return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate))); };
    kickDecay = decay(0.070);
    snareDecay = decay(0.045);
    hatDecay = decay(0.016);
    bassDecay = decay(0.155);
    kickBaseAdvance = static_cast<float>(50.0 / sampleRate);
    kickSweepAdvance = static_cast<float>(85.0 / sampleRate);
    for (std::size_t i = 0; i < bassRates.size(); ++i)
        bassRates[i] = static_cast<float>(65.406391 * std::exp2(static_cast<double>(i) / 12.0) / sampleRate);
    reset();
}

void Arranger::reset() noexcept
{
    running = false;
    active.store(-1, std::memory_order_relaxed);
    beat.store(0, std::memory_order_relaxed);
    kick = snare = hat = bass = 0;
}

void Arranger::audioDeviceStopped() { reset(); }

void Arranger::beginStep(int index) noexcept
{
    struct Pattern { unsigned kick, snare, bass; };
    constexpr std::array patterns {
        Pattern { 0b00010001, 0b01000100, 0b00010001 }, // Main A
        Pattern { 0b00101001, 0b01000100, 0b01010101 }, // Main B
        Pattern { 0b01000001, 0b10010000, 0b00010001 }, // Intro
        Pattern { 0b00100001, 0b11010100, 0b01000101 }, // Fill
        Pattern { 0b00010001, 0b00010100, 0b00010001 }, // Ending
        Pattern { 0b10010101, 0b01010100, 0b10101011 }, // Main C
        Pattern { 0b00000001, 0b00000001, 0b00000001 }  // Break
    };

    if (index == 0)
    {
        section = live.takeNextSection();
        if (section < 0)
        {
            reset();
            return;
        }
        active.store(section, std::memory_order_relaxed);
    }

    if (index % 2 == 0)
        beat.store(index / 2 + 1, std::memory_order_relaxed);
    if (section == breakSection && index >= 2)
    {
        kick = snare = hat = bass = 0;
        return;
    }
    const auto& pattern = patterns[static_cast<std::size_t>(section)];
    const auto bit = 1u << index;
    hat = (section == endingSection && index > 4) ? 0.0f : (index % 2 == 0 ? 0.55f : 0.85f);
    if ((pattern.snare & bit) != 0)
        snare = 1.0f;
    if ((pattern.kick & bit) != 0)
    {
        kick = 1.0f;
        kickPhase = 0.25f;
    }
    if ((pattern.bass & bit) != 0)
    {
        const auto chord = live.chord.load(std::memory_order_relaxed);
        const int root = chord >= 0 ? chord % 12 : 0;
        const auto quality = chord >= 0 ? chord / 12 : ChordDetector::major;
        constexpr std::array fifths { 7, 7, 7, 7, 7, 7, 7, 6, 8 };
        constexpr std::array accents { 4, 3, 10, 10, 11, 2, 5, 3, 4 };
        const auto chordQuality = static_cast<std::size_t>(quality);
        const int interval = index == 4 && section != endingSection ? fifths[chordQuality]
                           : index == 6 ? accents[chordQuality] : 0;
        bassAdvance = bassRates[static_cast<std::size_t>((root + interval) % 12)];
        bass = 1.0f;
        bassPhase = 0.25f;
    }
}

float Arranger::nextSample() noexcept
{
    noise ^= noise << 13;
    noise ^= noise >> 17;
    noise ^= noise << 5;
    const auto white = static_cast<float>(noise & 0xffffu) / 32768.0f - 1.0f;
    const auto kickLevel = envelope(kick, kickDecay);
    return 0.28f * kickLevel * triangle(kickPhase, kickBaseAdvance + kickSweepAdvance * kickLevel)
         + white * (0.18f * envelope(snare, snareDecay) + 0.07f * envelope(hat, hatDecay))
         + 0.24f * envelope(bass, bassDecay) * triangle(bassPhase, bassAdvance);
}

void Arranger::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
                                               int channels, int samples,
                                               const juce::AudioIODeviceCallbackContext&)
{
    if (!live.isPlaying())
    {
        if (running)
            reset();
        for (int channel = 0; channel < channels; ++channel)
            if (auto* output = outputs[channel])
                std::fill_n(output, samples, 0.0f);
        return;
    }

    if (!running)
    {
        running = true;
        step = 0;
        stepPhase = 0;
        beginStep(step);
    }

    if (live.takeImmediateFill() && running && section != endingSection)
    {
        section = fillSection;
        active.store(section, std::memory_order_relaxed);
        snare = 1.0f;
        hat = 0.60f;
    }

    const double stepAdvance = live.bpm() * 2.0 / (60.0 * sampleRate);
    for (int i = 0; i < samples; ++i)
    {
        const auto value = running ? nextSample() : 0.0f;
        for (int channel = 0; channel < channels; ++channel)
            if (auto* output = outputs[channel])
                output[i] = value;
        if (!running)
            continue;
        stepPhase += stepAdvance;
        if (stepPhase >= 1.0)
        {
            stepPhase -= 1.0;
            step = (step + 1) % stepsPerBar;
            if (step == 0 && section == endingSection)
            {
                live.stop();
                reset();
            }
            else
                beginStep(step);
        }
    }
}
