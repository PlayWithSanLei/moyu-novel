#include "app/Hotkeys.h"

#include <QStringList>

namespace moyu {
namespace {

constexpr unsigned short kVkAnsiQ = 0x0C;
constexpr unsigned short kVkAnsiE = 0x0E;
constexpr unsigned short kVkLeft = 0x7B;
constexpr unsigned short kVkRight = 0x7C;

}  // namespace

HotkeyChord defaultChord(HotkeyRole role)
{
    HotkeyChord chord;
    chord.qtModifiers = Qt::AltModifier;
    switch (role) {
    case HotkeyRole::Boss:
        chord.qtKey = Qt::Key_Q;
        chord.keyCode = kVkAnsiQ;
        break;
    case HotkeyRole::NextPage:
        chord.qtKey = Qt::Key_Right;
        chord.keyCode = kVkRight;
        break;
    case HotkeyRole::PreviousPage:
        chord.qtKey = Qt::Key_Left;
        chord.keyCode = kVkLeft;
        break;
    case HotkeyRole::ToggleClickThrough:
        chord.qtKey = Qt::Key_E;
        chord.keyCode = kVkAnsiE;
        break;
    }
    return chord;
}

QString chordLabel(const HotkeyChord& chord)
{
    const auto modifiers = static_cast<Qt::KeyboardModifiers>(chord.qtModifiers);
    QString text;
    if (modifiers.testFlag(Qt::MetaModifier)) {
        text += QStringLiteral("⌃");
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        text += QStringLiteral("⌥");
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        text += QStringLiteral("⇧");
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        text += QStringLiteral("⌘");
    }
    text += QKeySequence(chord.qtKey).toString(QKeySequence::NativeText);
    return text;
}

QString serializeChord(const HotkeyChord& chord)
{
    return QStringLiteral("%1,%2,%3").arg(chord.keyCode).arg(chord.qtKey).arg(chord.qtModifiers);
}

std::optional<HotkeyChord> parseChord(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 3) {
        return std::nullopt;
    }
    bool okCode = false;
    bool okKey = false;
    bool okMods = false;
    const int code = parts.at(0).toInt(&okCode);
    const int key = parts.at(1).toInt(&okKey);
    const int mods = parts.at(2).toInt(&okMods);
    if (!okCode || !okKey || !okMods || code <= 0 || code > 0xFFFF || key == 0) {
        return std::nullopt;
    }
    HotkeyChord chord;
    chord.keyCode = static_cast<unsigned short>(code);
    chord.qtKey = key;
    chord.qtModifiers = mods;
    if (!chord.valid()) {
        return std::nullopt;
    }
    return chord;
}

bool isReservedSystemHotkey(int qtKey, Qt::KeyboardModifiers modifiers)
{
    if (!modifiers.testFlag(Qt::ControlModifier)) {
        return false;
    }
    switch (qtKey) {
    case Qt::Key_Q:
    case Qt::Key_Tab:
    case Qt::Key_Space:
    case Qt::Key_H:
    case Qt::Key_M:
    case Qt::Key_W:
    case Qt::Key_QuoteLeft:
        return true;
    case Qt::Key_Escape:
        return modifiers.testFlag(Qt::AltModifier);
    default:
        return false;
    }
}

}  // namespace moyu
