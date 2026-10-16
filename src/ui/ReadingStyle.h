#pragma once

#include "app/StealthPolicy.h"

#include <QColor>
#include <QFont>
#include <QString>

#include <algorithm>
#include <vector>

namespace moyu {

enum class PagePreset {
    Dark,
    White,
    Green,
    Paper,
    Custom,
};

// 背景和文字的 alpha 分开算，不用 QWidget::setWindowOpacity。
struct ReadingStyle {
    PagePreset preset = PagePreset::Dark;
    QColor background{28, 28, 30};
    int backgroundOpacityPercent = 82;
    QColor text{255, 255, 255};
    int textOpacityPercent = 90;
    QString fontFamily;
    int fontPointSize = 15;
    int lineSpacingPercent = 100;
    int paragraphGap = 10;
    int margin = 16;

    QFont makeFont() const
    {
        QFont font;
        if (!fontFamily.isEmpty()) {
            font.setFamily(fontFamily);
        }
        font.setPointSize(std::clamp(fontPointSize, 8, 72));
        return font;
    }
};

inline QColor styledBackground(const ReadingStyle& style, bool clickThrough)
{
    QColor color = style.background;
    color.setAlpha(opacityAlpha(style.backgroundOpacityPercent, clickThrough));
    return color;
}

inline QColor styledInk(const ReadingStyle& style, bool clickThrough)
{
    const int percent = std::clamp(style.textOpacityPercent, 10, 100);
    QColor color = style.text;
    color.setAlpha(opacityAlpha(percent, clickThrough));
    return color;
}

QString presetName(PagePreset preset);
PagePreset presetFromName(const QString& name);
ReadingStyle presetStyle(PagePreset preset, const ReadingStyle& current);

std::vector<std::pair<QString, QString>> readingStyleItems(const ReadingStyle& style);
ReadingStyle readingStyleFromItems(const std::vector<std::pair<QString, QString>>& items, ReadingStyle base);

}  // namespace moyu
