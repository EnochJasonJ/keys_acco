#pragma once

#include "StyleTimeline.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

class TimelineGrid final : public juce::Component
{
public:
    explicit TimelineGrid(const StyleTimeline& timeline) : style(timeline)
    {
        setTitle("Style timeline");
        setDescription("Drag across bars to select a range. The first and last bar fields also set the range.");
    }

    static constexpr int labelWidth = 68;
    static constexpr int minimumBarWidth = 70;

    std::function<void(BarRange)> onSelection;

    void showSelection(BarRange range)
    {
        selection = range;
        repaint();
    }

    void showPlayhead(int bar)
    {
        if (playhead != bar)
        {
            playhead = bar;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.x >= labelWidth && event.mods.isLeftButtonDown())
        {
            anchor = barAt(event.x);
            select(anchor);
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (anchor >= 0)
            select(barAt(event.x));
    }

    void mouseUp(const juce::MouseEvent&) override { anchor = -1; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff242a32));
        g.setColour(juce::Colour(0xffdce4ed));
        g.drawText("Drums", 8, 24, labelWidth - 12, 23, juce::Justification::centredLeft);
        g.drawText("Bass", 8, 48, labelWidth - 12, 23, juce::Justification::centredLeft);

        const float width = barWidth();
        for (std::size_t i = 0; i < style.bars.size(); ++i)
        {
            const float x = labelWidth + static_cast<float>(i) * width;
            if (!g.getClipBounds().intersects(juce::Rectangle<float>(x, 0, width, 72).getSmallestIntegerContainer()))
                continue;
            const int bar = static_cast<int>(i);
            const bool selected = bar >= selection.first && bar <= selection.last;
            g.setColour(juce::Colour(selected ? 0xff344e65 : 0xff2b333d));
            g.fillRect(x, 0.0f, width - 1.0f, 72.0f);
            g.setColour(juce::Colour(0xffe6edf4));
            g.drawText(juce::String(bar + 1), juce::Rectangle<float>(x, 0, width, 22).toNearestInt(),
                       juce::Justification::centred);

            const auto& pattern = style.bars[i];
            paintHits(g, x, width, 29.0f, pattern.hat, juce::Colour(0xff587889), 2.0f);
            paintHits(g, x, width, 36.0f, pattern.kick | pattern.snare, juce::Colour(0xff7dd6c7), 6.0f);
            paintHits(g, x, width, 55.0f, pattern.bass, juce::Colour(0xffb9a7f1), 8.0f);
            if (playhead == bar)
            {
                g.setColour(juce::Colour(0xff77dec9));
                g.drawRect(x + 1.0f, 1.0f, width - 3.0f, 70.0f, 2.0f);
            }
        }
    }

private:
    float barWidth() const
    {
        return static_cast<float>(getWidth() - labelWidth) / static_cast<float>(style.bars.size());
    }

    int barAt(int x) const
    {
        return juce::jlimit(0, static_cast<int>(style.bars.size()) - 1,
                            static_cast<int>(static_cast<float>(x - labelWidth) / barWidth()));
    }

    void select(int bar) const
    {
        if (onSelection)
            onSelection({ std::min(anchor, bar), std::max(anchor, bar) });
    }

    static void paintHits(juce::Graphics& g, float x, float width, float y, unsigned mask,
                          juce::Colour colour, float height)
    {
        const auto step = (width - 8.0f) / 8.0f;
        g.setColour(colour);
        for (int i = 0; i < 8; ++i)
            if ((mask & (1u << i)) != 0)
                g.fillRect(x + 4.0f + static_cast<float>(i) * step, y, step - 1.0f, height);
    }

    const StyleTimeline& style;
    BarRange selection { 0, 0 };
    int anchor = -1;
    int playhead = -1;
};
