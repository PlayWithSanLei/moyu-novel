#include "app/Application.h"

#include "app/BookSession.h"
#include "parser/OpenBook.h"
#include "app/StartupTiming.h"
#include "app/StealthPolicy.h"
#include "core/Database.h"
#include "core/Paths.h"
#include "mac/GlobalHotkeys.h"
#include "mac/MacWindow.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/ShelfDialog.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QTimer>

#include <limits>

namespace moyu {
namespace {

QString settingValue(const std::vector<std::pair<QString, QString>>& items, const QString& key)
{
    for (const auto& item : items) {
        if (item.first == key) {
            return item.second;
        }
    }
    return {};
}

bool flagFrom(const QString& text, bool fallback)
{
    if (text == QLatin1String("1")) {
        return true;
    }
    if (text == QLatin1String("0")) {
        return false;
    }
    return fallback;
}

}  // namespace

Application::Application()
    : m_database(std::make_unique<Database>(databaseFilePath()))
{
    for (HotkeyRole role : {HotkeyRole::Boss, HotkeyRole::NextPage, HotkeyRole::PreviousPage, HotkeyRole::ToggleClickThrough}) {
        m_chords[chordSlot(role)] = defaultChord(role);
    }
    const bool opened = m_database->startAndWait();
    m_session = std::make_unique<BookSession>(m_database.get(), cacheDirectoryPath());
    m_window = std::make_unique<MainWindow>();
    m_hotkeys = new GlobalHotkeys(m_window.get());
    m_saveTimer = new QTimer(m_window.get());
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(300);
    QObject::connect(m_saveTimer, &QTimer::timeout, m_window.get(), [this] { savePreferences(); });
    createMenu();
    wire();
    if (!opened) {
        const QString message = m_database->errorString().isEmpty()
                                    ? QStringLiteral("无法打开数据库")
                                    : m_database->errorString();
        m_window->showToast(message);
    }
    registerHotkeys();
    loadPreferences();
    m_session->requestPlacement();
}

Application::~Application()
{
    if (m_dirty) {
        savePreferences();
    }
    if (m_session != nullptr) {
        m_session->flushNow();
    }
    delete m_settings;
    m_settings = nullptr;
    // 先停加载线程，再关窗口，最后关数据库。顺序反了会在错误的线程上碰 QObject。
    m_session.reset();
    m_window.reset();
    m_database.reset();
}

int Application::run()
{
    m_window->show();
    syncStatusItem();
    // AppKit 在 finishLaunching 里装默认的重开处理器，排到事件循环开始之后再覆盖。
    QTimer::singleShot(0, m_window.get(), [this] {
        MacWindow::installReopenHandler([this] {
            if (m_window->isBossHidden() || !m_window->isVisible()) {
                m_window->showForBoss();
            }
            MacWindow::makeKey(m_window.get());
        });
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [this] { m_session->flushNow(); });
    const QStringList args = QCoreApplication::arguments();
    if (args.size() > 1) {
        m_session->openFile(args.at(1));
    } else {
        m_session->openLast();
    }
    return QApplication::exec();
}

int Application::chordSlot(HotkeyRole role) const
{
    switch (role) {
    case HotkeyRole::Boss:
        return 0;
    case HotkeyRole::NextPage:
        return 1;
    case HotkeyRole::PreviousPage:
        return 2;
    case HotkeyRole::ToggleClickThrough:
        return 3;
    }
    return 0;
}

void Application::openTextFile()
{
    const QStringList paths = QFileDialog::getOpenFileNames(m_window.get(),
                                                            QStringLiteral("打开"),
                                                            QString(),
                                                            supportedBookFilter());
    if (!paths.isEmpty()) {
        m_session->openFiles(paths);
    }
}

void Application::showShelf()
{
    if (m_shelf == nullptr) {
        m_shelf = new ShelfDialog(m_window.get());
        QObject::connect(m_shelf, &ShelfDialog::importRequested, m_window.get(), [this] { openTextFile(); });
        QObject::connect(m_shelf, &ShelfDialog::openRequested, m_window.get(), [this](qint64 id) { openShelfEntry(id); });
        QObject::connect(m_shelf, &ShelfDialog::removeRequested, m_window.get(), [this](qint64 id) {
            m_session->removeFromShelf(id);
        });
    }
    refreshShelf();
    m_shelf->show();
    m_shelf->raise();
}

void Application::refreshShelf()
{
    if (m_shelf == nullptr || m_database == nullptr || !m_database->isOpen()) {
        return;
    }
    auto rows = m_database->listShelf();
    if (!rows) {
        m_window->showToast(rows.error().message);
        return;
    }
    m_shelfRows = std::move(*rows);
    m_shelf->setEntries(m_shelfRows);
}

void Application::openShelfEntry(qint64 bookId)
{
    const ShelfEntry* entry = nullptr;
    for (const ShelfEntry& row : m_shelfRows) {
        if (row.id == bookId) {
            entry = &row;
            break;
        }
    }
    if (entry == nullptr) {
        return;
    }
    if (!QFileInfo::exists(entry->filePath)) {
        askRelocate(entry->id, entry->title, entry->filePath);
        return;
    }
    m_session->openFile(entry->filePath);
}

void Application::askRelocate(qint64 bookId, const QString& title, const QString& oldPath)
{
    const QString caption = title.isEmpty() ? QStringLiteral("找不到原文件，请重新选择")
                                             : QStringLiteral("找不到《%1》，请重新选择").arg(title);
    const QString picked = QFileDialog::getOpenFileName(m_window.get(),
                                                        caption,
                                                        QFileInfo(oldPath).absolutePath(),
                                                        supportedBookFilter());
    if (picked.isEmpty()) {
        m_window->showToast(QStringLiteral("找不到原文件"));
        return;
    }
    auto moved = m_database->retargetBook(bookId, picked);
    if (!moved) {
        m_window->showToast(moved.error().message);
        return;
    }
    m_session->openFile(picked);
}

void Application::createMenu()
{
    auto* bar = new QMenuBar(m_window.get());
    bar->setNativeMenuBar(true);

    QMenu* fileMenu = bar->addMenu(QStringLiteral("文件"));
    QAction* open = fileMenu->addAction(QStringLiteral("打开…"));
    open->setShortcut(QKeySequence::Open);
    QObject::connect(open, &QAction::triggered, m_window.get(), [this] { openTextFile(); });
    QAction* shelf = fileMenu->addAction(QStringLiteral("书架…"));
    QObject::connect(shelf, &QAction::triggered, m_window.get(), [this] { showShelf(); });

    QMenu* windowMenu = bar->addMenu(QStringLiteral("窗口"));
    m_pin = windowMenu->addAction(QStringLiteral("窗口置顶"));
    m_pin->setCheckable(true);
    QObject::connect(m_pin, &QAction::toggled, m_window.get(), [this](bool on) {
        if (m_status.pin != nullptr && m_status.pin->isChecked() != on) {
            m_status.pin->setChecked(on);
        }
        m_window->setAlwaysOnTop(on);
        m_session->setAlwaysOnTop(on);
    });
    m_clickAction = windowMenu->addAction(QStringLiteral("鼠标穿透"));
    m_clickAction->setCheckable(true);
    QObject::connect(m_clickAction, &QAction::toggled, m_window.get(), [this](bool on) {
        if (!m_syncing) {
            requestClickThrough(on);
        }
    });
    m_dockAction = windowMenu->addAction(QStringLiteral("不在 Dock 显示"));
    m_dockAction->setCheckable(true);
    QObject::connect(m_dockAction, &QAction::toggled, m_window.get(), [this](bool on) {
        if (!m_syncing) {
            requestHideDock(on);
        }
    });
    QAction* settings = windowMenu->addAction(QStringLiteral("设置…"));
    QObject::connect(settings, &QAction::triggered, m_window.get(), [this] { showSettings(); });

    QMenu* readMenu = bar->addMenu(QStringLiteral("阅读"));
    m_pageAction = readMenu->addAction(QStringLiteral("翻页模式"));
    m_pageAction->setCheckable(true);
    QObject::connect(m_pageAction, &QAction::toggled, m_window.get(), [this](bool on) {
        if (m_syncing || m_loading) {
            return;
        }
        m_window->setPageMode(on);
        scheduleSave();
    });
    m_autoAction = readMenu->addAction(QStringLiteral("自动滚动"));
    m_autoAction->setCheckable(true);
    QObject::connect(m_autoAction, &QAction::toggled, m_window.get(), [this](bool on) {
        if (m_syncing || m_loading) {
            return;
        }
        m_window->setAutoScroll(on);
        scheduleSave();
    });
    QAction* jump = readMenu->addAction(QStringLiteral("跳转…"));
    jump->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    jump->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(jump, &QAction::triggered, m_window.get(), [this] { m_window->showJump(); });
    QAction* search = readMenu->addAction(QStringLiteral("搜索"));
    search->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));
    search->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(search, &QAction::triggered, m_window.get(), [this] { m_window->showSearch(); });
    QAction* back = readMenu->addAction(QStringLiteral("返回上一位置"));
    back->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    back->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(back, &QAction::triggered, m_window.get(), [this] { m_window->goBack(); });
    QAction* addMark = readMenu->addAction(QStringLiteral("添加书签"));
    addMark->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    addMark->setShortcutContext(Qt::WidgetShortcut);
    QObject::connect(addMark, &QAction::triggered, m_window.get(), [this] { m_window->addBookmarkHere(); });
    QAction* marks = readMenu->addAction(QStringLiteral("书签…"));
    QObject::connect(marks, &QAction::triggered, m_window.get(), [this] { m_window->showBookmarks(); });

    m_status = createStatusMenu(m_window.get());
    QObject::connect(m_status.showWindow, &QAction::triggered, m_window.get(), [this] { m_window->showForBoss(); });
    QObject::connect(m_status.openFile, &QAction::triggered, m_window.get(), [this] { openTextFile(); });
    QObject::connect(m_status.closeClickThrough, &QAction::triggered, m_window.get(), [this] {
        requestClickThrough(false);
    });
    QObject::connect(m_status.pin, &QAction::toggled, m_pin, &QAction::setChecked);
    QObject::connect(m_status.settings, &QAction::triggered, m_window.get(), [this] { showSettings(); });
    QObject::connect(m_status.quit, &QAction::triggered, qApp, &QCoreApplication::quit);
}

void Application::wire()
{
    QObject::connect(m_window.get(), &MainWindow::openFileRequested, m_session.get(), &BookSession::openFile);
    QObject::connect(m_window.get(), &MainWindow::openFilesRequested, m_session.get(), &BookSession::openFiles);
    QObject::connect(m_window.get(), &MainWindow::chapterActivated, m_session.get(), [this](quint32 index) {
        m_session->goToChapter(index, false);
    });
    // 滚过边界只预读紧挨着的一章，拼进当前文档。整章替换留给目录、搜索和百分比跳转。
    QObject::connect(m_window.get(), &MainWindow::nextChapterRequested, m_session.get(), [this] {
        const quint32 last = m_window->lastLoadedChapter();
        if (last == std::numeric_limits<quint32>::max()) {
            return;
        }
        m_session->requestNeighbor(last + 1);
    });
    QObject::connect(m_window.get(), &MainWindow::previousChapterRequested, m_session.get(), [this] {
        const quint32 first = m_window->firstLoadedChapter();
        if (first == 0) {
            return;
        }
        m_session->requestNeighbor(first - 1);
    });
    QObject::connect(m_window.get(), &MainWindow::anchorJumpRequested, m_session.get(), [this](quint32 chapter, quint32 offset) {
        m_session->goToAnchor(chapter, offset);
    });
    QObject::connect(m_window.get(), &MainWindow::searchRequested, m_session.get(), [this](const QString& text) {
        m_session->search(text);
    });
    QObject::connect(m_window.get(), &MainWindow::searchCancelRequested, m_session.get(), [this] { m_session->cancelSearch(); });
    QObject::connect(m_window.get(),
                     &MainWindow::bookmarkAddRequested,
                     m_session.get(),
                     [this](quint32 chapter, quint32 offset, const QString& preview) {
                         m_session->addBookmark(chapter, offset, preview);
                         m_session->requestBookmarks();
                     });
    QObject::connect(m_window.get(), &MainWindow::bookmarkDeleteRequested, m_session.get(), [this](qint64 id) {
        m_session->removeBookmark(id);
    });
    QObject::connect(m_window.get(), &MainWindow::bookmarksRequested, m_session.get(), [this] { m_session->requestBookmarks(); });
    QObject::connect(m_window.get(), &MainWindow::anchorMoved, m_session.get(), &BookSession::noteAnchor);
    QObject::connect(m_window.get(), &MainWindow::geometryChanged, m_session.get(), &BookSession::noteGeometry);
    QObject::connect(m_window.get(), &MainWindow::bossHidden, m_session.get(), [this] { m_session->flushNow(); });

    QObject::connect(m_hotkeys, &GlobalHotkeys::triggered, m_window.get(), [this](int role) {
        const auto effect = effectForHotkey(static_cast<HotkeyRole>(role), m_window->isBossHidden());
        switch (effect) {
        case HotkeyEffect::ToggleBoss:
            if (m_window->isBossHidden()) {
                m_window->showForBoss();
            } else {
                m_window->hideForBoss();
            }
            break;
        case HotkeyEffect::PageNext:
            m_window->turnPageFromHotkey(true);
            break;
        case HotkeyEffect::PagePrevious:
            m_window->turnPageFromHotkey(false);
            break;
        case HotkeyEffect::ToggleClickThrough:
            requestClickThrough(!m_window->clickThrough());
            break;
        case HotkeyEffect::Ignore:
            break;
        }
    });

    QObject::connect(m_session.get(), &BookSession::previewReady, m_window.get(), &MainWindow::showPreview);
    QObject::connect(m_session.get(), &BookSession::bookReady, m_window.get(), [this](const TxtDocument& document) {
        if (document.chapterIndex >= document.parsed.chapters.size()) {
            noteIndexReady();
            return;
        }
        const ChapterIndex& chapter = document.parsed.chapters[document.chapterIndex];
        m_window->showBook(document.parsed.chapters,
                           document.chapter,
                           chapter.byteStart,
                           document.parsed.book.totalBytes,
                           document.byteOffset);
        if (m_shelf != nullptr && m_shelf->isVisible()) {
            refreshShelf();
        }
        noteIndexReady();
    });
    QObject::connect(m_session.get(),
                     &BookSession::chapterReady,
                     m_window.get(),
                     [this](const ChapterContent& content,
                            quint64 chapterByteStart,
                            quint64 totalBytes,
                            quint32 byteOffset,
                            bool pinToEnd) {
                         m_window->showChapter(content, chapterByteStart, totalBytes, byteOffset, pinToEnd);
                     });
    QObject::connect(m_session.get(),
                     &BookSession::neighborReady,
                     m_window.get(),
                     [this](const ChapterContent& content, quint64 chapterByteStart, quint64 totalBytes) {
                         m_window->attachNeighbor(content, chapterByteStart, totalBytes);
                     });
    QObject::connect(m_session.get(),
                     &BookSession::searchBatch,
                     m_window.get(),
                     [this](const std::vector<SearchHitInfo>& hits, bool done) { m_window->addSearchHits(hits, done); });
    QObject::connect(m_session.get(), &BookSession::bookmarksReady, m_window.get(), [this](const std::vector<Bookmark>& rows) {
        m_window->setBookmarkList(rows);
    });
    QObject::connect(m_session.get(), &BookSession::fileMissing, m_window.get(), [this](qint64 bookId, const QString& title, const QString& path) {
        askRelocate(bookId, title, path);
        noteIndexReady();
    });
    QObject::connect(m_session.get(), &BookSession::bookCleared, m_window.get(), [this] { m_window->clearBook(); });
    QObject::connect(m_session.get(), &BookSession::shelfChanged, m_window.get(), [this] { refreshShelf(); });
    QObject::connect(m_session.get(), &BookSession::openFailed, m_window.get(), [this](const QString& message, bool clear) {
        if (clear) {
            m_window->clearBook();
        }
        m_window->showToast(message);
        noteIndexReady();
    });
    QObject::connect(m_session.get(), &BookSession::placementReady, m_window.get(), [this](const WindowPlacement& place) {
        if (place.hasGeometry) {
            m_window->setGeometry(place.geometry);
        }
        if (place.hasOnTop && m_pin != nullptr) {
            m_pin->setChecked(place.onTop);
        }
    });
}

void Application::loadPreferences()
{
    if (m_database == nullptr || !m_database->isOpen()) {
        return;
    }
    const std::vector<QString> keys = {
        QStringLiteral("appearance/preset"),
        QStringLiteral("appearance/bgColor"),
        QStringLiteral("appearance/bgOpacity"),
        QStringLiteral("appearance/textColor"),
        QStringLiteral("appearance/textOpacity"),
        QStringLiteral("appearance/fontFamily"),
        QStringLiteral("appearance/fontSize"),
        QStringLiteral("appearance/lineSpacing"),
        QStringLiteral("appearance/paragraphGap"),
        QStringLiteral("appearance/margin"),
        QStringLiteral("window/clickThrough"),
        QStringLiteral("window/hideDock"),
        QStringLiteral("window/hideOnBlur"),
        QStringLiteral("hotkey/boss"),
        QStringLiteral("hotkey/next"),
        QStringLiteral("hotkey/previous"),
        QStringLiteral("hotkey/clickThrough"),
        QStringLiteral("reading/pageMode"),
        QStringLiteral("reading/autoScroll"),
        QStringLiteral("chapters/customRegex"),
    };
    m_database->requestSettings(m_window.get(), keys, [this](const std::vector<std::pair<QString, QString>>& items) {
        m_loading = true;
        m_style = readingStyleFromItems(items, m_style);
        m_window->applyAppearance(m_style);
        const auto takeChord = [&](const QString& key, HotkeyRole role) {
            const auto parsed = parseChord(settingValue(items, key));
            if (!parsed.has_value()) {
                return;
            }
            if (isReservedSystemHotkey(parsed->qtKey, static_cast<Qt::KeyboardModifiers>(parsed->qtModifiers))) {
                return;
            }
            m_chords[chordSlot(role)] = *parsed;
        };
        takeChord(QStringLiteral("hotkey/boss"), HotkeyRole::Boss);
        takeChord(QStringLiteral("hotkey/next"), HotkeyRole::NextPage);
        takeChord(QStringLiteral("hotkey/previous"), HotkeyRole::PreviousPage);
        takeChord(QStringLiteral("hotkey/clickThrough"), HotkeyRole::ToggleClickThrough);
        registerHotkeys();
        m_hideOnBlur = flagFrom(settingValue(items, QStringLiteral("window/hideOnBlur")), false);
        m_window->setHideOnBlur(m_hideOnBlur);
        if (flagFrom(settingValue(items, QStringLiteral("window/hideDock")), false)) {
            requestHideDock(true);
        }
        if (flagFrom(settingValue(items, QStringLiteral("window/clickThrough")), false)) {
            requestClickThrough(true);
        }
        m_chapterPattern = settingValue(items, QStringLiteral("chapters/customRegex"));
        m_session->setChapterPattern(m_chapterPattern);
        const bool pageMode = flagFrom(settingValue(items, QStringLiteral("reading/pageMode")), false);
        const bool autoScroll = flagFrom(settingValue(items, QStringLiteral("reading/autoScroll")), false);
        m_window->setPageMode(pageMode);
        m_window->setAutoScroll(autoScroll);
        m_syncing = true;
        if (m_pageAction != nullptr) {
            m_pageAction->setChecked(pageMode);
        }
        if (m_autoAction != nullptr) {
            m_autoAction->setChecked(autoScroll);
        }
        m_syncing = false;
        if (m_settings != nullptr) {
            m_settings->setReadingStyle(m_style);
            m_settings->setHideOnBlur(m_hideOnBlur);
            m_settings->setChapterPattern(m_chapterPattern);
            for (HotkeyRole role : {HotkeyRole::Boss, HotkeyRole::NextPage, HotkeyRole::PreviousPage, HotkeyRole::ToggleClickThrough}) {
                m_settings->setHotkey(role, m_chords[chordSlot(role)]);
            }
        }
        m_loading = false;
    });
}

void Application::scheduleSave()
{
    if (m_loading || m_saveTimer == nullptr) {
        return;
    }
    m_dirty = true;
    m_saveTimer->start();
}

void Application::savePreferences()
{
    if (m_database == nullptr || !m_database->isOpen()) {
        return;
    }
    m_dirty = false;
    auto items = readingStyleItems(m_style);
    items.emplace_back(QStringLiteral("window/clickThrough"), m_window->clickThrough() ? QStringLiteral("1") : QStringLiteral("0"));
    items.emplace_back(QStringLiteral("window/hideDock"), m_hideDock ? QStringLiteral("1") : QStringLiteral("0"));
    items.emplace_back(QStringLiteral("window/hideOnBlur"), m_hideOnBlur ? QStringLiteral("1") : QStringLiteral("0"));
    items.emplace_back(QStringLiteral("hotkey/boss"), serializeChord(m_chords[0]));
    items.emplace_back(QStringLiteral("hotkey/next"), serializeChord(m_chords[1]));
    items.emplace_back(QStringLiteral("hotkey/previous"), serializeChord(m_chords[2]));
    items.emplace_back(QStringLiteral("hotkey/clickThrough"), serializeChord(m_chords[3]));
    items.emplace_back(QStringLiteral("reading/pageMode"), m_window->pageMode() ? QStringLiteral("1") : QStringLiteral("0"));
    items.emplace_back(QStringLiteral("reading/autoScroll"), m_window->autoScroll() ? QStringLiteral("1") : QStringLiteral("0"));
    items.emplace_back(QStringLiteral("chapters/customRegex"), m_chapterPattern);
    m_database->setSettingsAsync(items);
}

void Application::registerHotkeys()
{
    QStringList failed;
    if (!m_hotkeys->bind(HotkeyRole::Boss, m_chords[0])) {
        failed.push_back(QStringLiteral("老板键"));
    }
    if (!m_hotkeys->bind(HotkeyRole::NextPage, m_chords[1])) {
        failed.push_back(QStringLiteral("下一页"));
    }
    if (!m_hotkeys->bind(HotkeyRole::PreviousPage, m_chords[2])) {
        failed.push_back(QStringLiteral("上一页"));
    }
    m_clickHotkeyReady = m_hotkeys->bind(HotkeyRole::ToggleClickThrough, m_chords[3]);
    if (!m_clickHotkeyReady) {
        failed.push_back(QStringLiteral("切换穿透"));
        if (m_window->clickThrough()) {
            requestClickThrough(false);
        }
    }
    if (!failed.isEmpty() && !m_loading) {
        m_window->showToast(failed.join(QStringLiteral("、")) + QStringLiteral("注册失败，请换一个组合键。"));
    }
}

bool Application::applyHotkey(HotkeyRole role, const HotkeyChord& chord)
{
    if (isReservedSystemHotkey(chord.qtKey, static_cast<Qt::KeyboardModifiers>(chord.qtModifiers)) || !chord.valid()) {
        m_window->showToast(QStringLiteral("不能录 Cmd+Q、Cmd+Tab 这类系统组合键"));
        return false;
    }
    const HotkeyChord previous = m_chords[chordSlot(role)];
    if (!m_hotkeys->bind(role, chord)) {
        m_hotkeys->bind(role, previous);
        m_window->showToast(QStringLiteral("快捷键注册失败，请换一个。可能被别的程序占用了。"));
        return false;
    }
    m_chords[chordSlot(role)] = chord;
    if (role == HotkeyRole::ToggleClickThrough) {
        m_clickHotkeyReady = true;
    }
    scheduleSave();
    return true;
}

void Application::requestClickThrough(bool on)
{
    if (!on) {
        m_window->setClickThrough(false);
        if (m_status.closeClickThrough != nullptr) {
            m_status.closeClickThrough->setEnabled(false);
        }
        m_syncing = true;
        if (m_clickAction != nullptr) {
            m_clickAction->setChecked(false);
        }
        if (m_settings != nullptr) {
            m_settings->setClickThrough(false);
        }
        m_syncing = false;
        syncStatusItem();
        scheduleSave();
        return;
    }
    const bool trayReady = ensureStatusItem();
    if (decideClickThrough(true, m_clickHotkeyReady, trayReady) != ClickThroughDecision::TurnOn) {
        m_window->setClickThrough(false);
        m_syncing = true;
        if (m_clickAction != nullptr) {
            m_clickAction->setChecked(false);
        }
        if (m_settings != nullptr) {
            m_settings->setClickThrough(false);
        }
        m_syncing = false;
        if (!m_clickHotkeyReady) {
            m_window->showToast(QStringLiteral("「切换穿透」的快捷键注册失败，穿透没有打开。请换一个组合键。"));
        } else {
            m_window->showToast(QStringLiteral("菜单栏图标没有就绪，穿透没有打开。"));
        }
        return;
    }
    m_window->setClickThrough(true);
    if (m_status.closeClickThrough != nullptr) {
        m_status.closeClickThrough->setEnabled(true);
    }
    m_syncing = true;
    if (m_clickAction != nullptr) {
        m_clickAction->setChecked(true);
    }
    if (m_settings != nullptr) {
        m_settings->setClickThrough(true);
    }
    m_syncing = false;
    syncStatusItem();
    scheduleSave();
}

void Application::requestHideDock(bool hide)
{
    if (hide && !ensureStatusItem()) {
        m_window->showToast(QStringLiteral("菜单栏图标没有就绪，没有隐藏 Dock。"));
        m_syncing = true;
        if (m_dockAction != nullptr) {
            m_dockAction->setChecked(false);
        }
        if (m_settings != nullptr) {
            m_settings->setHideFromDock(false);
        }
        m_syncing = false;
        return;
    }
    m_hideDock = hide;
    MacWindow::setHidesFromDock(m_window.get(), hide, m_window->alwaysOnTop());
    m_syncing = true;
    if (m_dockAction != nullptr) {
        m_dockAction->setChecked(hide);
    }
    if (m_settings != nullptr) {
        m_settings->setHideFromDock(hide);
    }
    m_syncing = false;
    syncStatusItem();
    scheduleSave();
}

bool Application::ensureStatusItem()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return false;
    }
    if (m_tray == nullptr) {
        m_tray = new QSystemTrayIcon(m_window.get());
        QPixmap pix(22, 22);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(28, 28, 30));
        painter.drawRoundedRect(pix.rect().adjusted(1, 1, -1, -1), 4, 4);
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        painter.drawText(pix.rect(), Qt::AlignCenter, QStringLiteral("摸"));
        m_tray->setIcon(QIcon(pix));
        m_tray->setToolTip(QStringLiteral("摸鱼阅读器"));
        m_tray->setContextMenu(m_status.menu);
    }
    m_tray->show();
    return true;
}

void Application::syncStatusItem()
{
    // 菜单栏图标常驻，退出应用才消失。Command+W 关掉窗口之后，它和 Dock 图标都能把窗口叫回来。
    ensureStatusItem();
}

void Application::showSettings()
{
    if (m_settings == nullptr) {
        m_settings = new SettingsDialog();
        QObject::connect(m_settings, &SettingsDialog::appearanceEdited, m_window.get(), [this](const ReadingStyle& style) {
            m_style = style;
            m_window->applyAppearance(style);
            scheduleSave();
        });
        QObject::connect(m_settings, &SettingsDialog::clickThroughRequested, m_window.get(), [this](bool on) {
            requestClickThrough(on);
        });
        QObject::connect(m_settings, &SettingsDialog::hideDockRequested, m_window.get(), [this](bool on) {
            requestHideDock(on);
        });
        QObject::connect(m_settings, &SettingsDialog::hideOnBlurChanged, m_window.get(), [this](bool on) {
            m_hideOnBlur = on;
            m_window->setHideOnBlur(on);
            scheduleSave();
        });
        QObject::connect(m_settings, &SettingsDialog::hotkeyEdited, m_window.get(), [this](int role, const HotkeyChord& chord) {
            const auto hotkeyRole = static_cast<HotkeyRole>(role);
            if (!applyHotkey(hotkeyRole, chord) && m_settings != nullptr) {
                m_settings->setHotkey(hotkeyRole, m_chords[chordSlot(hotkeyRole)]);
            }
        });
        QObject::connect(m_settings, &SettingsDialog::hotkeyRejected, m_window.get(), [this](const QString& message) {
            m_window->showToast(message);
        });
        QObject::connect(m_settings, &SettingsDialog::chapterPatternEdited, m_window.get(), [this](const QString& pattern) {
            m_chapterPattern = pattern;
            m_session->setChapterPattern(pattern);
            scheduleSave();
        });
        QObject::connect(m_settings, &SettingsDialog::regenerateRequested, m_window.get(), [this] {
            m_session->regenerateChapters();
        });
        QObject::connect(m_settings, &QDialog::finished, m_window.get(), [this] {
            m_window->setSuspendBlurHide(false);
        });
    }
    m_settings->setReadingStyle(m_style);
    m_settings->setClickThrough(m_window->clickThrough());
    m_settings->setHideFromDock(m_hideDock);
    m_settings->setHideOnBlur(m_hideOnBlur);
    m_settings->setChapterPattern(m_chapterPattern);
    for (HotkeyRole role : {HotkeyRole::Boss, HotkeyRole::NextPage, HotkeyRole::PreviousPage, HotkeyRole::ToggleClickThrough}) {
        m_settings->setHotkey(role, m_chords[chordSlot(role)]);
    }
    m_window->setSuspendBlurHide(true);
    m_settings->show();
    m_settings->raise();
}

}  // namespace moyu
