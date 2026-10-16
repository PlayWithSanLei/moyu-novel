#pragma once

class QAction;
class QMenu;
class QWidget;

namespace moyu {

struct StatusMenu {
    QMenu* menu = nullptr;
    QAction* showWindow = nullptr;
    QAction* openFile = nullptr;
    QAction* closeClickThrough = nullptr;
    QAction* pin = nullptr;
    QAction* settings = nullptr;
    QAction* quit = nullptr;
};

// Dock 藏起来之后系统菜单栏也会消失。退出、设置、关闭穿透、置顶都放在这个菜单里。
StatusMenu createStatusMenu(QWidget* parent);

}  // namespace moyu
