#pragma once

#include "core/Types.h"

#include <QDialog>
#include <QList>
#include <QStringList>
#include <QUrl>

class QTableWidget;
class QUrl;

namespace moyu {

// 拖进来的本地路径。远程链接丢掉。
QStringList localFilePaths(const QList<QUrl>& urls);

// 非模态。顺序沿用调用方排好的最后阅读时间。
class ShelfDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShelfDialog(QWidget* parent = nullptr);

    void setEntries(const std::vector<ShelfEntry>& entries);

Q_SIGNALS:
    void openRequested(qint64 bookId);
    void removeRequested(qint64 bookId);
    void importRequested();

private:
    qint64 selectedId() const;
    void openSelection();

    QTableWidget* m_table = nullptr;
    std::vector<qint64> m_ids;
};

}  // namespace moyu
