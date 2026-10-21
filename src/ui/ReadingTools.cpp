#include "ui/ReadingTools.h"

#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace moyu {

SearchPanel::SearchPanel(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("搜索"));
    setModal(false);
    resize(420, 480);
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(QStringLiteral("在全文里查找"));
    auto* search = new QPushButton(QStringLiteral("搜索"), this);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    // 回车由输入框的 returnPressed 处理；按钮再当默认按钮会把同一次搜索发两遍。
    search->setAutoDefault(false);
    cancel->setAutoDefault(false);
    row->addWidget(m_query, 1);
    row->addWidget(search);
    row->addWidget(cancel);
    layout->addLayout(row);
    m_results = new QListWidget(this);
    layout->addWidget(m_results, 1);
    connect(search, &QPushButton::clicked, this, [this] { submit(); });
    connect(m_query, &QLineEdit::returnPressed, this, [this] { submit(); });
    // 停掉正在跑的搜索交给 hideEvent。
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_results, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        Q_EMIT hitChosen(static_cast<quint32>(item->data(Qt::UserRole).toUInt()),
                         static_cast<quint32>(item->data(Qt::UserRole + 1).toUInt()),
                         m_needleBytes);
    });
}

void SearchPanel::submit()
{
    const QString text = m_query->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    m_results->clear();
    m_needleBytes = static_cast<quint32>(text.toUtf8().size());
    m_running = true;
    Q_EMIT searchRequested(text);
}

void SearchPanel::addHits(const std::vector<SearchHitInfo>& hits)
{
    for (const SearchHitInfo& hit : hits) {
        auto* item = new QListWidgetItem(QStringLiteral("第 %1 章  %2").arg(hit.chapterIndex + 1).arg(hit.preview));
        item->setData(Qt::UserRole, hit.chapterIndex);
        item->setData(Qt::UserRole + 1, hit.byteOffset);
        m_results->addItem(item);
    }
}

void SearchPanel::markFinished()
{
    m_running = false;
    if (m_results->count() == 0) {
        m_results->addItem(QStringLiteral("没有匹配"));
    }
}

void SearchPanel::hideEvent(QHideEvent* event)
{
    if (m_running) {
        m_running = false;
        Q_EMIT cancelRequested();
    }
    Q_EMIT dismissed();
    QDialog::hideEvent(event);
}

JumpDialog::JumpDialog(int chapterCount, int currentPercent, int currentChapter, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("跳转"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("百分比"), this));
    auto* percentRow = new QHBoxLayout();
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 100);
    m_percent = new QSpinBox(this);
    m_percent->setRange(0, 100);
    m_percent->setSuffix(QStringLiteral("%"));
    const int percent = std::clamp(currentPercent, 0, 100);
    m_slider->setValue(percent);
    m_percent->setValue(percent);
    percentRow->addWidget(m_slider, 1);
    percentRow->addWidget(m_percent);
    layout->addLayout(percentRow);
    connect(m_slider, &QSlider::valueChanged, m_percent, &QSpinBox::setValue);
    connect(m_percent, qOverload<int>(&QSpinBox::valueChanged), m_slider, &QSlider::setValue);

    layout->addWidget(new QLabel(QStringLiteral("章节序号"), this));
    m_chapter = new QSpinBox(this);
    m_chapter->setRange(1, std::max(1, chapterCount));
    m_chapter->setValue(std::clamp(currentChapter, 1, std::max(1, chapterCount)));
    layout->addWidget(m_chapter);

    auto* buttons = new QHBoxLayout();
    auto* byPercent = new QPushButton(QStringLiteral("跳到百分比"), this);
    auto* byChapter = new QPushButton(QStringLiteral("跳到章节"), this);
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    buttons->addWidget(byPercent);
    buttons->addWidget(byChapter);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(byPercent, &QPushButton::clicked, this, [this] {
        m_accepted = true;
        m_byPercent = true;
        accept();
    });
    connect(byChapter, &QPushButton::clicked, this, [this] {
        m_accepted = true;
        m_byPercent = false;
        accept();
    });
    connect(close, &QPushButton::clicked, this, [this] { reject(); });
}

int JumpDialog::percent() const
{
    return m_percent != nullptr ? m_percent->value() : 0;
}

int JumpDialog::chapterNumber() const
{
    return m_chapter != nullptr ? m_chapter->value() : 1;
}

BookmarkDialog::BookmarkDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("书签"));
    resize(420, 360);
    auto* layout = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);
    auto* row = new QHBoxLayout();
    auto* open = new QPushButton(QStringLiteral("跳转"), this);
    auto* remove = new QPushButton(QStringLiteral("删除"), this);
    auto* dismiss = new QPushButton(QStringLiteral("关闭"), this);
    row->addWidget(open);
    row->addWidget(remove);
    row->addWidget(dismiss);
    layout->addLayout(row);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { activate(item); });
    connect(open, &QPushButton::clicked, this, [this] { activate(m_list->currentItem()); });
    connect(remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(dismiss, &QPushButton::clicked, this, [this] { hide(); });
}

void BookmarkDialog::setBookmarks(const std::vector<Bookmark>& bookmarks)
{
    m_list->clear();
    for (const Bookmark& mark : bookmarks) {
        auto* item = new QListWidgetItem(QStringLiteral("第 %1 章  %2").arg(mark.chapterIndex + 1).arg(mark.preview));
        item->setData(Qt::UserRole, static_cast<qulonglong>(mark.id));
        item->setData(Qt::UserRole + 1, mark.chapterIndex);
        item->setData(Qt::UserRole + 2, mark.byteOffsetInChapter);
        m_list->addItem(item);
    }
    if (m_list->count() == 0) {
        m_list->addItem(QStringLiteral("还没有书签"));
    }
}

void BookmarkDialog::activate(QListWidgetItem* item)
{
    if (item == nullptr || !item->data(Qt::UserRole).isValid() || item->data(Qt::UserRole).toLongLong() <= 0) {
        return;
    }
    Q_EMIT openRequested(static_cast<quint32>(item->data(Qt::UserRole + 1).toUInt()),
                         static_cast<quint32>(item->data(Qt::UserRole + 2).toUInt()));
}

void BookmarkDialog::removeSelected()
{
    QListWidgetItem* item = m_list->currentItem();
    if (item == nullptr) {
        return;
    }
    const qint64 id = item->data(Qt::UserRole).toLongLong();
    if (id <= 0) {
        return;
    }
    delete item;
    Q_EMIT deleteRequested(id);
}

}  // namespace moyu
