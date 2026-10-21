#pragma once

#include "app/BookSession.h"
#include "core/Types.h"

#include <QDialog>

#include <vector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QSpinBox;

namespace moyu {

class SearchPanel : public QDialog {
    Q_OBJECT

public:
    explicit SearchPanel(QWidget* parent = nullptr);

    void addHits(const std::vector<SearchHitInfo>& hits);
    void markFinished();

Q_SIGNALS:
    void searchRequested(QString text);
    void cancelRequested();
    // 面板关掉就算这次搜索结束了，取消按钮、Esc、关闭窗口都走这里。
    void dismissed();
    void hitChosen(quint32 chapter, quint32 byteOffset, quint32 needleBytes);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void submit();

    QLineEdit* m_query = nullptr;
    QListWidget* m_results = nullptr;
    quint32 m_needleBytes = 0;
    bool m_running = false;
};

class JumpDialog : public QDialog {
    Q_OBJECT

public:
    JumpDialog(int chapterCount, int currentPercent, int currentChapter, QWidget* parent = nullptr);

    bool acceptedJump() const { return m_accepted; }
    bool byPercent() const { return m_byPercent; }
    int percent() const;
    int chapterNumber() const;

private:
    bool m_accepted = false;
    bool m_byPercent = true;
    QSlider* m_slider = nullptr;
    QSpinBox* m_percent = nullptr;
    QSpinBox* m_chapter = nullptr;
};

class BookmarkDialog : public QDialog {
    Q_OBJECT

public:
    explicit BookmarkDialog(QWidget* parent = nullptr);

    void setBookmarks(const std::vector<Bookmark>& bookmarks);

Q_SIGNALS:
    void openRequested(quint32 chapter, quint32 byteOffset);
    void deleteRequested(qint64 id);

private:
    void activate(QListWidgetItem* item);
    void removeSelected();

    QListWidget* m_list = nullptr;
};

}  // namespace moyu
