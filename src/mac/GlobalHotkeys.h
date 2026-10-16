#pragma once

#include "app/Hotkeys.h"

#include <QObject>

namespace moyu {

// Carbon RegisterEventHotKey。不需要辅助功能权限，应用不在前台也能收到。
// 同一个组合被占用时 bind 返回 false，调用方不要据此打开穿透。
class GlobalHotkeys : public QObject {
    Q_OBJECT

public:
    explicit GlobalHotkeys(QObject* parent = nullptr);
    ~GlobalHotkeys() override;

    bool bind(HotkeyRole role, const HotkeyChord& chord);
    void unbind(HotkeyRole role);
    void unbindAll();

    void dispatch(int role);

Q_SIGNALS:
    void triggered(int role);
};

}  // namespace moyu
