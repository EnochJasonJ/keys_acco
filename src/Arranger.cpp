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
    visibleBar.store(-1, std::memory_order_relaxed);
    kick = snare = hat = bass = 0;
}

void Arranger::audioDeviceStopped() { reset(); }

void Arranger::enterSection(int next) noexcept
{
    section = next;
    const auto range = ranges[static_cast<std::size_t>(next)].load(std::memory_order_relaxed);
    sectionRange = { range / StyleTimeline::maxBars, range % StyleTimeline::maxBars };
    bar = sectionRange.first;
    active.store(section, std::memory_order_relaxed);
    visibleBar.store(bar, std::memory_order_relaxed);
}

void Arranger::beginBar(bool starting) noexcept
{
    if (!starting && section == endingSection)
    {
        if (bar == sectionRange.last)
        {
            live.stop();
            reset();
        }
        else
            visibleBar.store(++bar, std::memory_order_relaxed);
        return;
    }

    const int queued = live.takeQueuedSection();
    if (queued < 0)
    {
        reset();
        return;
    }

    const int selected = live.requested.load(std::memory_order_relaxed);
    const bool isMain = section == mainASection || section == mainBSection || section == mainCSection;
    if (queued != 0)
        enterSection(queued);
    else if (starting || (isMain && selected != section) || bar == sectionRange.last)
        enterSection(selected);
    else
        visibleBar.store(++bar, std::memory_order_relaxed);
}

void Arranger::beginStep(int index) noexcept
{
    if (index % 2 == 0)
        beat.store(index / 2 + 1, std::memory_order_relaxed);
    const auto& pattern = bars[static_cast<std::size_t>(bar)];
    const auto bit = 1u << index;
    if ((pattern.cutTails & bit) != 0)
    {
        kick = snare = hat = bass = 0;
        return;
    }
    hat = (pattern.hat & bit) != 0 ? (index % 2 == 0 ? 0.55f : 0.85f) : 0.0f;
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
        const int interval = index == 4 && pattern.bassFifthOnBeatThree ? fifths[chordQuality]
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
        beginBar(true);
        if (running)
            beginStep(step);
    }

    if (live.takeImmediateFill() && running && section != endingSection)
    {
        enterSection(fillSection);
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
            if (step == 0)
                beginBar(false);
            if (running)
                beginStep(step);
        }
    }
}
