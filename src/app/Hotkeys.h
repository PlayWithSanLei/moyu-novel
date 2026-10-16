#pragma once

#include <QKeySequence>
#include <QString>

#include <optional>

namespace moyu {

// Qt 在 macOS 上把 Command 记成 ControlModifier，把 Control 记成 MetaModifier。
enum class HotkeyRole {
    Boss = 1,
    NextPage = 2,
    PreviousPage = 3,
    ToggleClickThrough = 4,
};

enum class HotkeyEffect {
    ToggleBoss,
    PageNext,
    PagePrevious,
    ToggleClickThrough,
    Ignore,
};

struct HotkeyChord {
    int qtKey = 0;
    int qtModifiers = 0;
    unsigned short keyCode = 0;

    bool valid() const { return qtKey != 0 && keyCode != 0 && qtModifiers != 0; }
};

HotkeyChord defaultChord(HotkeyRole role);
QString chordLabel(const HotkeyChord& chord);
QString serializeChord(const HotkeyChord& chord);
std::optional<HotkeyChord> parseChord(const QString& text);

// Cmd+Q、Cmd+Tab，以及 Cmd+Space / Cmd+H / Cmd+M / Cmd+W / Cmd+` / Cmd+Option+Esc。
// Option+Q 是老板键的默认值，不在这里面。
bool isReservedSystemHotkey(int qtKey, Qt::KeyboardModifiers modifiers);

// 窗口藏着的时候，翻页键直接丢掉，不能把窗口叫出来。老板键和穿透键仍然有效。
inline HotkeyEffect effectForHotkey(HotkeyRole role, bool bossHidden)
{
    switch (role) {
    case HotkeyRole::Boss:
        return HotkeyEffect::ToggleBoss;
    case HotkeyRole::ToggleClickThrough:
        return HotkeyEffect::ToggleClickThrough;
    case HotkeyRole::NextPage:
        return bossHidden ? HotkeyEffect::Ignore : HotkeyEffect::PageNext;
    case HotkeyRole::PreviousPage:
        return bossHidden ? HotkeyEffect::Ignore : HotkeyEffect::PagePrevious;
    }
    return HotkeyEffect::Ignore;
}

}  // namespace moyu
