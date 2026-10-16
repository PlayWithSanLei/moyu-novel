#pragma once

#include "core/Types.h"

#include <QColor>
#include <QWidget>

#include <vector>

class QLineEdit;
class QTreeView;
class QSortFilterProxyModel;

namespace moyu {

class TocModel;

// 侧边目录。卷（level 1）收纳后面的章，直到下一卷。
// 打开时滚到当前章，不把焦点抢到过滤框。
class TocDrawer : public QWidget {
    Q_OBJECT

public:
    explicit TocDrawer(QWidget* parent = nullptr);

    void setChapters(const std::vector<ChapterIndex>& chapters);
    void setCurrentChapter(quint32 chapter);
    void revealCurrent();
    void setColors(const QColor& base, const QColor& ink);

Q_SIGNALS:
    void chapterActivated(quint32 chapterIndex);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void layoutChildren();

    TocModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    QLineEdit* m_filter = nullptr;
    QTreeView* m_tree = nullptr;
    quint32 m_current = 0;
    bool m_hasCurrent = false;
};

}  // namespace moyu
