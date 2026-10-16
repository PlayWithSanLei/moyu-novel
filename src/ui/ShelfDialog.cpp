#include "ui/ShelfDialog.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace moyu {

QStringList localFilePaths(const QList<QUrl>& urls)
{
    QStringList paths;
    for (const QUrl& url : urls) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty()) {
            paths.push_back(path);
        }
    }
    return paths;
}

ShelfDialog::ShelfDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("书架"));
    resize(520, 360);
    auto* layout = new QVBoxLayout(this);
    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({QStringLiteral("书名"), QStringLiteral("进度"), QStringLiteral("最后阅读")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout();
    auto* importButton = new QPushButton(QStringLiteral("导入…"), this);
    auto* openButton = new QPushButton(QStringLiteral("打开"), this);
    auto* removeButton = new QPushButton(QStringLiteral("删除"), this);
    buttons->addWidget(importButton);
    buttons->addStretch(1);
    buttons->addWidget(openButton);
    buttons->addWidget(removeButton);
    layout->addLayout(buttons);

    connect(importButton, &QPushButton::clicked, this, [this] { Q_EMIT importRequested(); });
    connect(openButton, &QPushButton::clicked, this, [this] { openSelection(); });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        const qint64 id = selectedId();
        if (id > 0) {
            Q_EMIT removeRequested(id);
        }
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { openSelection(); });
}

void ShelfDialog::setEntries(const std::vector<ShelfEntry>& entries)
{
    const qint64 keep = selectedId();
    m_ids.clear();
    m_table->setRowCount(static_cast<int>(entries.size()));
    int selectRow = -1;
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const ShelfEntry& entry = entries[static_cast<std::size_t>(row)];
        m_ids.push_back(entry.id);
        const bool missing = !entry.filePath.isEmpty() && !QFileInfo::exists(entry.filePath);
        const QString title = missing ? entry.title + QStringLiteral("（文件不在）") : entry.title;
        const QString when = entry.lastReadAt > 0
                                 ? QDateTime::fromMSecsSinceEpoch(entry.lastReadAt).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                 : QString();
        m_table->setItem(row, 0, new QTableWidgetItem(title));
        m_table->setItem(row, 1, new QTableWidgetItem(QString::number(entry.percent) + QStringLiteral("%")));
        m_table->setItem(row, 2, new QTableWidgetItem(when));
        if (entry.id == keep) {
            selectRow = row;
        }
    }
    if (selectRow >= 0) {
        m_table->selectRow(selectRow);
    }
}

qint64 ShelfDialog::selectedId() const
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= static_cast<int>(m_ids.size())) {
        return 0;
    }
    return m_ids[static_cast<std::size_t>(row)];
}

void ShelfDialog::openSelection()
{
    const qint64 id = selectedId();
    if (id > 0) {
        Q_EMIT openRequested(id);
    }
}

}  // namespace moyu
