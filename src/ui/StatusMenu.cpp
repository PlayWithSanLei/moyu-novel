#include "ui/StatusMenu.h"

#include <QAction>
#include <QMenu>

namespace moyu {

StatusMenu createStatusMenu(QWidget* parent)
{
    StatusMenu handles;
    handles.menu = new QMenu(parent);
    handles.showWindow = handles.menu->addAction(QStringLiteral("显示窗口"));
    handles.openFile = handles.menu->addAction(QStringLiteral("打开…"));
    handles.closeClickThrough = handles.menu->addAction(QStringLiteral("关闭穿透"));
    handles.closeClickThrough->setEnabled(false);
    handles.pin = handles.menu->addAction(QStringLiteral("窗口置顶"));
    handles.pin->setCheckable(true);
    handles.settings = handles.menu->addAction(QStringLiteral("设置…"));
    handles.menu->addSeparator();
    handles.quit = handles.menu->addAction(QStringLiteral("退出"));
    return handles;
}

}  // namespace moyu
