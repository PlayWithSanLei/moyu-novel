#include "ui/TocDrawer.h"

#include "ui/Theme.h"

#include <QAbstractItemModel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVariant>

#include <algorithm>
#include <vector>

namespace moyu {
namespace {

struct Node {
    int parent = -1;
    int row = 0;
    int chapter = -1;
    QString title;
    std::vector<int> children;
};

}  // namespace

class TocModel : public QAbstractItemModel {
public:
    explicit TocModel(QObject* parent = nullptr)
        : QAbstractItemModel(parent)
    {
    }

    void setChapters(const std::vector<ChapterIndex>& chapters)
    {
        beginResetModel();
        m_nodes.clear();
        m_roots.clear();
        m_byChapter.assign(chapters.size(), -1);
        m_nodes.reserve(chapters.size());
        int volume = -1;
        for (const ChapterIndex& chapter : chapters) {
            Node node;
            node.chapter = static_cast<int>(chapter.index);
            node.title = chapter.title;
            node.parent = (chapter.level <= 1 || volume < 0) ? -1 : volume;
            const int id = static_cast<int>(m_nodes.size());
            if (node.parent < 0) {
                node.row = static_cast<int>(m_roots.size());
            } else {
                node.row = static_cast<int>(m_nodes[static_cast<std::size_t>(node.parent)].children.size());
            }
            m_nodes.push_back(std::move(node));
            if (m_nodes.back().parent < 0) {
                m_roots.push_back(id);
            } else {
                m_nodes[static_cast<std::size_t>(m_nodes.back().parent)].children.push_back(id);
            }
            if (chapter.index < m_byChapter.size()) {
                m_byChapter[chapter.index] = id;
            }
            if (chapter.level <= 1) {
                volume = id;
            }
        }
        endResetModel();
    }

    int chapterFor(const QModelIndex& index) const
    {
        if (!index.isValid()) {
            return -1;
        }
        const auto id = static_cast<int>(index.internalId());
        if (id < 0 || id >= static_cast<int>(m_nodes.size())) {
            return -1;
        }
        return m_nodes[static_cast<std::size_t>(id)].chapter;
    }

    QModelIndex indexForChapter(quint32 chapter) const
    {
        if (chapter >= m_byChapter.size()) {
            return {};
        }
        const int id = m_byChapter[chapter];
        if (id < 0) {
            return {};
        }
        const Node& node = m_nodes[static_cast<std::size_t>(id)];
        return createIndex(node.row, 0, static_cast<quintptr>(id));
    }

    QModelIndex index(int row, int column, const QModelIndex& parent) const override
    {
        if (column != 0 || row < 0) {
            return {};
        }
        const std::vector<int>& kids = childrenOf(parent);
        if (row >= static_cast<int>(kids.size())) {
            return {};
        }
        return createIndex(row, column, static_cast<quintptr>(kids[static_cast<std::size_t>(row)]));
    }

    QModelIndex parent(const QModelIndex& child) const override
    {
        if (!child.isValid()) {
            return {};
        }
        const auto id = static_cast<int>(child.internalId());
        if (id < 0 || id >= static_cast<int>(m_nodes.size())) {
            return {};
        }
        const int parentId = m_nodes[static_cast<std::size_t>(id)].parent;
        if (parentId < 0 || parentId >= static_cast<int>(m_nodes.size())) {
            return {};
        }
        const Node& node = m_nodes[static_cast<std::size_t>(parentId)];
        return createIndex(node.row, 0, static_cast<quintptr>(parentId));
    }

    int rowCount(const QModelIndex& parent) const override
    {
        if (parent.column() > 0) {
            return 0;
        }
        return static_cast<int>(childrenOf(parent).size());
    }

    int columnCount(const QModelIndex& /*parent*/) const override { return 1; }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole) {
            return {};
        }
        const auto id = static_cast<int>(index.internalId());
        if (id < 0 || id >= static_cast<int>(m_nodes.size())) {
            return {};
        }
        return m_nodes[static_cast<std::size_t>(id)].title;
    }

private:
    const std::vector<int>& childrenOf(const QModelIndex& parent) const
    {
        if (!parent.isValid()) {
            return m_roots;
        }
        const auto id = static_cast<int>(parent.internalId());
        if (id < 0 || id >= static_cast<int>(m_nodes.size())) {
            return m_empty;
        }
        return m_nodes[static_cast<std::size_t>(id)].children;
    }

    std::vector<Node> m_nodes;
    std::vector<int> m_roots;
    std::vector<int> m_byChapter;
    std::vector<int> m_empty;
};

TocDrawer::TocDrawer(QWidget* parent)
    : QWidget(parent)
    , m_model(new TocModel(this))
    , m_proxy(new QSortFilterProxyModel(this))
    , m_filter(new QLineEdit(this))
    , m_tree(new QTreeView(this))
{
    setAutoFillBackground(true);
    setColors(QColor(22, 22, 24, 235), kInkColor);

    m_filter->setPlaceholderText(QStringLiteral("过滤章节"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setFrame(false);

    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setFilterKeyColumn(0);

    m_tree->setModel(m_proxy);
    m_tree->setUniformRowHeights(true);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(false);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_proxy->setFilterFixedString(text);
        m_tree->expandAll();
    });
    connect(m_tree, &QTreeView::clicked, this, [this](const QModelIndex& proxyIndex) {
        const int chapter = m_model->chapterFor(m_proxy->mapToSource(proxyIndex));
        if (chapter >= 0) {
            Q_EMIT chapterActivated(static_cast<quint32>(chapter));
        }
    });
}

void TocDrawer::setColors(const QColor& base, const QColor& ink)
{
    QPalette pal = palette();
    pal.setColor(QPalette::Window, base);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::Text, ink);
    QColor placeholder = ink;
    placeholder.setAlpha(std::min(255, ink.alpha() / 2));
    pal.setColor(QPalette::PlaceholderText, placeholder);
    pal.setColor(QPalette::Highlight, QColor(64, 64, 68));
    pal.setColor(QPalette::HighlightedText, ink);
    setPalette(pal);
    if (m_filter != nullptr) {
        m_filter->setPalette(pal);
    }
    if (m_tree != nullptr) {
        m_tree->setPalette(pal);
    }
}

void TocDrawer::setChapters(const std::vector<ChapterIndex>& chapters)
{
    m_model->setChapters(chapters);
    m_tree->expandAll();
    if (m_hasCurrent) {
        revealCurrent();
    }
}

void TocDrawer::setCurrentChapter(quint32 chapter)
{
    m_current = chapter;
    m_hasCurrent = true;
    if (isVisible()) {
        revealCurrent();
    }
}

void TocDrawer::revealCurrent()
{
    const QModelIndex source = m_model->indexForChapter(m_current);
    const QModelIndex proxyIndex = m_proxy->mapFromSource(source);
    if (!proxyIndex.isValid()) {
        return;
    }
    if (proxyIndex.parent().isValid()) {
        m_tree->expand(proxyIndex.parent());
    }
    m_tree->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
    m_tree->setCurrentIndex(proxyIndex);
}

void TocDrawer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutChildren();
}

void TocDrawer::layoutChildren()
{
    constexpr int kFilterHeight = 28;
    m_filter->setGeometry(0, 0, width(), kFilterHeight);
    m_tree->setGeometry(0, kFilterHeight, width(), std::max(0, height() - kFilterHeight));
}

}  // namespace moyu
