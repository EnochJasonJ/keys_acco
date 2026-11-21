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

void Arranger::replaceTimeline(const StyleTimeline& timeline)
{
    bars = timeline.bars;
    for (std::size_t i = 0; i < timeline.assignments.size(); ++i)
        setRange(i, timeline.assignments[i]);

    for (auto& list : events)
        list.clear();
    const int end = static_cast<int>(bars.size()) * StyleTimeline::ticksPerBar;
    for (std::size_t i = 0; i < timeline.notes.size(); ++i)
    {
        const auto& note = timeline.notes[i];
        const auto append = [&](int tick, int velocity)
        {
            if (tick < end)
                events[static_cast<std::size_t>(tick / StyleTimeline::ticksPerBar)]
                    .push_back({ tick % StyleTimeline::ticksPerBar, note.track, note.pitch, velocity, static_cast<int>(i) });
        };
        append(note.tick, note.velocity);
        if (note.track == bassTrack)
            append(note.tick + note.length, 0);
    }
    for (auto& list : events)
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b)
        {
            return a.tick < b.tick || (a.tick == b.tick && a.velocity == 0 && b.velocity != 0);
        });
}

void Arranger::prepareRecording(BarRange range, int track)
{
    recordingRange = range;
    recordingTrack = track;
    recordEpoch.store(0, std::memory_order_relaxed);
    recordEndEpoch.store(0, std::memory_order_relaxed);
}

void Arranger::updateTimelineBar(bool contiguous) noexcept
{
    eventCursor = 0;
    if (!contiguous && bassVoice >= 0)
    {
        bassVoice = -1;
        bass = 0;
    }
    visibleBar.store(bar, std::memory_order_relaxed);
}

bool Arranger::trackEnabled(int track) const noexcept
{
    return !renderingRecording || track != recordingTrack;
}

void Arranger::beginRecordingBar(bool starting, double outputTimeMs) noexcept
{
    if (starting)
    {
        countingIn = true;
        visibleBar.store(-1, std::memory_order_relaxed);
        return;
    }

    if (countingIn)
    {
        countingIn = false;
        bar = recordingRange.first;
        updateTimelineBar(false);
        recordEpoch.store(outputTimeMs, std::memory_order_release);
    }
    else if (bar == recordingRange.last)
    {
        recordEndEpoch.store(outputTimeMs, std::memory_order_release);
        reset();
    }
    else
    {
        ++bar;
        updateTimelineBar(true);
    }
}

void Arranger::triggerClick(int index) noexcept
{
    const bool accent = index == 0;
    click = accent ? 0.20f : 0.13f;
    clickAdvance = static_cast<float>((accent ? 1550.0 : 1070.0) / sampleRate);
    clickPhase = 0.25f;
}

void Arranger::playRecordedEvents(double tick) noexcept
{
    const auto& list = events[static_cast<std::size_t>(bar)];
    while (eventCursor < list.size() && list[eventCursor].tick <= tick)
    {
        const auto& event = list[eventCursor++];
        if (!trackEnabled(event.track))
            continue;

        const float velocity = static_cast<float>(event.velocity) / 127.0f;
        if (event.track == drumsTrack)
        {
            if (event.pitch == kickNotes[0] || event.pitch == kickNotes[1])
            {
                kick = velocity;
                kickPhase = 0.25f;
            }
            else if (event.pitch == snareNotes[0] || event.pitch == snareNotes[1])
                snare = velocity;
            else
                hat = velocity;
        }
        else if (event.velocity != 0)
        {
            const int chord = renderingRecording ? -1 : live.chord.load(std::memory_order_relaxed);
            const int root = chord >= 0 ? chord % 12 : 0;
            const auto quality = static_cast<std::size_t>(chord >= 0 ? chord / 12 : ChordDetector::major);
            constexpr std::array thirds { 4, 3, 4, 3, 4, 2, 5, 3, 4 };
            constexpr std::array fifths { 7, 7, 7, 7, 7, 7, 7, 6, 8 };
            int pitch = event.pitch + root;
            if (event.pitch % 12 == 4)
                pitch += thirds[quality] - 4;
            else if (event.pitch % 12 == 7)
                pitch += fifths[quality] - 7;
            bassAdvance = noteRates[static_cast<std::size_t>(std::clamp(pitch, 0, 127))];
            bass = velocity;
            bassPhase = 0.25f;
            bassVoice = event.id;
        }
        else if (bassVoice == event.id)
            bassVoice = -1;
    }
}

void Arranger::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    outputLatencyMs = 1000.0 * std::max(0, device->getOutputLatencyInSamples()) / sampleRate;
    const auto decay = [&](double seconds) { return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate))); };
    clickDecay = decay(0.010);
    kickDecay = decay(0.070);
    snareDecay = decay(0.045);
    hatDecay = decay(0.016);
    bassDecay = decay(0.155);
    kickBaseAdvance = static_cast<float>(50.0 / sampleRate);
    kickSweepAdvance = static_cast<float>(85.0 / sampleRate);
    for (std::size_t i = 0; i < bassRates.size(); ++i)
        bassRates[i] = static_cast<float>(65.406391 * std::exp2(static_cast<double>(i) / 12.0) / sampleRate);
    for (std::size_t i = 0; i < noteRates.size(); ++i)
        noteRates[i] = static_cast<float>(440.0 * std::exp2((static_cast<double>(i) - 69.0) / 12.0) / sampleRate);
    reset();
}

void Arranger::reset() noexcept
{
    running = false;
    countingIn = false;
    click = 0;
    bassVoice = -1;
    active.store(-1, std::memory_order_relaxed);
    beat.store(0, std::memory_order_relaxed);
    visibleBar.store(-1, std::memory_order_relaxed);
    visibleTick.store(-1, std::memory_order_relaxed);
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
    updateTimelineBar(false);
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
        {
            ++bar;
            updateTimelineBar(true);
        }
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
    {
        ++bar;
        updateTimelineBar(true);
    }
}

void Arranger::beginStep(int index) noexcept
{
    if (index % 2 == 0)
    {
        beat.store(index / 2 + 1, std::memory_order_relaxed);
        if (renderingRecording)
            triggerClick(index);
    }
    if (countingIn)
        return;
    const auto& pattern = bars[static_cast<std::size_t>(bar)];
    const auto bit = 1u << index;
    const bool cut = (pattern.cutTails & bit) != 0;
    if (trackEnabled(drumsTrack) && (pattern.recordedTracks & (1u << drumsTrack)) == 0)
    {
        if (cut)
            kick = snare = hat = 0;
        else
        {
            hat = (pattern.hat & bit) != 0 ? (index % 2 == 0 ? 0.55f : 0.85f) : 0.0f;
            if ((pattern.snare & bit) != 0)
                snare = 1.0f;
            if ((pattern.kick & bit) != 0)
            {
                kick = 1.0f;
                kickPhase = 0.25f;
            }
        }
    }
    if (!trackEnabled(bassTrack) || (pattern.recordedTracks & (1u << bassTrack)) != 0)
        return;
    if (cut)
    {
        bassVoice = -1;
        bass = 0;
    }
    else if ((pattern.bass & bit) != 0)
    {
        bassVoice = -1;
        const auto chord = renderingRecording ? -1 : live.chord.load(std::memory_order_relaxed);
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
    const auto metronome = click > 0 ? envelope(click, clickDecay) * triangle(clickPhase, clickAdvance) : 0.0f;
    return metronome + 0.28f * kickLevel * triangle(kickPhase, kickBaseAdvance + kickSweepAdvance * kickLevel)
         + white * (0.18f * envelope(snare, snareDecay) + 0.07f * envelope(hat, hatDecay))
         + 0.24f * (bassVoice >= 0 ? bass : envelope(bass, bassDecay)) * triangle(bassPhase, bassAdvance);
}

void Arranger::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
                                               int channels, int samples,
                                               const juce::AudioIODeviceCallbackContext& context)
{
    const bool record = live.recording.load(std::memory_order_acquire) != 0;
    if (record != renderingRecording)
    {
        reset();
        renderingRecording = record;
    }
    if (record ? recordEndEpoch.load(std::memory_order_relaxed) > 0 : !live.isPlaying())
    {
        if (running)
            reset();
        for (int channel = 0; channel < channels; ++channel)
            if (auto* output = outputs[channel])
                std::fill_n(output, samples, 0.0f);
        return;
    }

    double outputStartMs = 0;
    if (record)
    {
        // CoreAudio reports the output buffer's host time in the same clock domain
        // as MIDI arrival. Add the sample offset when the downbeat falls inside a buffer.
#if JUCE_MAC
        if (context.hostTimeNs != nullptr && *context.hostTimeNs != 0)
            outputStartMs = static_cast<double>(*context.hostTimeNs) / 1000000.0;
#endif
        if (outputStartMs == 0)
            outputStartMs = juce::Time::getMillisecondCounterHiRes() + outputLatencyMs;
    }

    if (!running)
    {
        running = true;
        step = 0;
        stepPhase = 0;
        if (record)
            beginRecordingBar(true, outputStartMs);
        else
            beginBar(true);
        if (running)
            beginStep(step);
    }

    constexpr int ticksPerStep = StyleTimeline::ticksPerBar / stepsPerBar;
    if (!record && live.takeImmediateFill() && running && section != endingSection)
    {
        enterSection(fillSection);
        const double position = (step + stepPhase) * ticksPerStep;
        const auto& list = events[static_cast<std::size_t>(bar)];
        while (eventCursor < list.size() && list[eventCursor].tick < position)
            ++eventCursor;
        if ((bars[static_cast<std::size_t>(bar)].recordedTracks & (1u << drumsTrack)) == 0)
        {
            snare = 1.0f;
            hat = 0.60f;
        }
    }

    const double stepAdvance = live.bpm() * 2.0 / (60.0 * sampleRate);
    for (int i = 0; i < samples; ++i)
    {
        if (running && !countingIn && eventCursor < events[static_cast<std::size_t>(bar)].size())
            playRecordedEvents((step + stepPhase) * ticksPerStep);
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
            {
                if (record)
                    beginRecordingBar(false, outputStartMs + (i + 1) * 1000.0 / sampleRate);
                else
                    beginBar(false);
            }
            if (running)
                beginStep(step);
        }
    }
    const int position = running && !countingIn
        ? bar * StyleTimeline::ticksPerBar + static_cast<int>((step + stepPhase) * ticksPerStep) : -1;
    visibleTick.store(position, std::memory_order_relaxed);
}
