#include "ui/ReadingStyle.h"

namespace moyu {
namespace {

QString colorText(const QColor& color)
{
    return QStringLiteral("#%1%2%3")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'));
}

QColor colorFrom(const QString& text, const QColor& fallback)
{
    const QColor parsed(text);
    if (!parsed.isValid()) {
        return fallback;
    }
    return QColor(parsed.red(), parsed.green(), parsed.blue());
}

int clampedInt(const QString& text, int fallback, int minValue, int maxValue)
{
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, minValue, maxValue);
}

QString itemValue(const std::vector<std::pair<QString, QString>>& items, const QString& key)
{
    for (const auto& item : items) {
        if (item.first == key) {
            return item.second;
        }
    }
    return {};
}

}  // namespace

QString presetName(PagePreset preset)
{
    switch (preset) {
    case PagePreset::Dark:
        return QStringLiteral("dark");
    case PagePreset::White:
        return QStringLiteral("white");
    case PagePreset::Green:
        return QStringLiteral("green");
    case PagePreset::Paper:
        return QStringLiteral("paper");
    case PagePreset::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("dark");
}

PagePreset presetFromName(const QString& name)
{
    if (name == QLatin1String("white")) {
        return PagePreset::White;
    }
    if (name == QLatin1String("green")) {
        return PagePreset::Green;
    }
    if (name == QLatin1String("paper")) {
        return PagePreset::Paper;
    }
    if (name == QLatin1String("custom")) {
        return PagePreset::Custom;
    }
    return PagePreset::Dark;
}

ReadingStyle presetStyle(PagePreset preset, const ReadingStyle& current)
{
    ReadingStyle style = current;
    style.preset = preset;
    switch (preset) {
    case PagePreset::Dark:
        style.background = QColor(28, 28, 30);
        style.text = QColor(255, 255, 255);
        break;
    case PagePreset::White:
        style.background = QColor(255, 255, 255);
        style.text = QColor(32, 32, 32);
        break;
    case PagePreset::Green:
        style.background = QColor(199, 237, 204);
        style.text = QColor(40, 48, 40);
        break;
    case PagePreset::Paper:
        style.background = QColor(244, 236, 216);
        style.text = QColor(72, 48, 24);
        break;
    case PagePreset::Custom:
        break;
    }
    return style;
}

std::vector<std::pair<QString, QString>> readingStyleItems(const ReadingStyle& style)
{
    return {
        {QStringLiteral("appearance/preset"), presetName(style.preset)},
        {QStringLiteral("appearance/bgColor"), colorText(style.background)},
        {QStringLiteral("appearance/bgOpacity"), QString::number(style.backgroundOpacityPercent)},
        {QStringLiteral("appearance/textColor"), colorText(style.text)},
        {QStringLiteral("appearance/textOpacity"), QString::number(style.textOpacityPercent)},
        {QStringLiteral("appearance/fontFamily"), style.fontFamily},
        {QStringLiteral("appearance/fontSize"), QString::number(style.fontPointSize)},
        {QStringLiteral("appearance/lineSpacing"), QString::number(style.lineSpacingPercent)},
        {QStringLiteral("appearance/paragraphGap"), QString::number(style.paragraphGap)},
        {QStringLiteral("appearance/margin"), QString::number(style.margin)},
    };
}

ReadingStyle readingStyleFromItems(const std::vector<std::pair<QString, QString>>& items, ReadingStyle base)
{
    const QString preset = itemValue(items, QStringLiteral("appearance/preset"));
    if (!preset.isEmpty()) {
        base.preset = presetFromName(preset);
    }
    const QString background = itemValue(items, QStringLiteral("appearance/bgColor"));
    if (!background.isEmpty()) {
        base.background = colorFrom(background, base.background);
    }
    const QString text = itemValue(items, QStringLiteral("appearance/textColor"));
    if (!text.isEmpty()) {
        base.text = colorFrom(text, base.text);
    }
    const QString bgOpacity = itemValue(items, QStringLiteral("appearance/bgOpacity"));
    if (!bgOpacity.isEmpty()) {
        base.backgroundOpacityPercent = clampedInt(bgOpacity, base.backgroundOpacityPercent, 0, 100);
    }
    const QString textOpacity = itemValue(items, QStringLiteral("appearance/textOpacity"));
    if (!textOpacity.isEmpty()) {
        base.textOpacityPercent = clampedInt(textOpacity, base.textOpacityPercent, 10, 100);
    }
    const QString family = itemValue(items, QStringLiteral("appearance/fontFamily"));
    if (!family.isNull()) {
        base.fontFamily = family;
    }
    const QString fontSize = itemValue(items, QStringLiteral("appearance/fontSize"));
    if (!fontSize.isEmpty()) {
        base.fontPointSize = clampedInt(fontSize, base.fontPointSize, 8, 72);
    }
    const QString spacing = itemValue(items, QStringLiteral("appearance/lineSpacing"));
    if (!spacing.isEmpty()) {
        base.lineSpacingPercent = clampedInt(spacing, base.lineSpacingPercent, 80, 200);
    }
    const QString gap = itemValue(items, QStringLiteral("appearance/paragraphGap"));
    if (!gap.isEmpty()) {
        base.paragraphGap = clampedInt(gap, base.paragraphGap, 0, 48);
    }
    const QString margin = itemValue(items, QStringLiteral("appearance/margin"));
    if (!margin.isEmpty()) {
        base.margin = clampedInt(margin, base.margin, 8, 64);
    }
    return base;
}

}  // namespace moyu
