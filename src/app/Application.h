#pragma once

#include "app/Hotkeys.h"
#include "core/Types.h"
#include "ui/ReadingStyle.h"
#include "ui/StatusMenu.h"

#include <memory>
#include <vector>

class QAction;
class QSystemTrayIcon;
class QTimer;

namespace moyu {

class BookSession;
class Database;
class GlobalHotkeys;
class MainWindow;
class SettingsDialog;
class ShelfDialog;

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    void createMenu();
    void wire();
    void loadPreferences();
    void savePreferences();
    void scheduleSave();
    void registerHotkeys();
    void showSettings();
    void requestClickThrough(bool on);
    void requestHideDock(bool hide);
    bool applyHotkey(HotkeyRole role, const HotkeyChord& chord);
    bool ensureStatusItem();
    void syncStatusItem();
    void openTextFile();
    void showShelf();
    void refreshShelf();
    void openShelfEntry(qint64 bookId);
    void askRelocate(qint64 bookId, const QString& title, const QString& oldPath);
    int chordSlot(HotkeyRole role) const;

    std::unique_ptr<Database> m_database;
    std::unique_ptr<BookSession> m_session;
    std::unique_ptr<MainWindow> m_window;
    QAction* m_pin = nullptr;
    QAction* m_clickAction = nullptr;
    QAction* m_dockAction = nullptr;
    QAction* m_pageAction = nullptr;
    QAction* m_autoAction = nullptr;
    StatusMenu m_status;
    QSystemTrayIcon* m_tray = nullptr;
    GlobalHotkeys* m_hotkeys = nullptr;
    SettingsDialog* m_settings = nullptr;
    ShelfDialog* m_shelf = nullptr;
    std::vector<ShelfEntry> m_shelfRows;
    QTimer* m_saveTimer = nullptr;
    ReadingStyle m_style;
    QString m_chapterPattern;
    HotkeyChord m_chords[4];
    bool m_clickHotkeyReady = false;
    bool m_hideDock = false;
    bool m_hideOnBlur = false;
    bool m_loading = false;
    bool m_dirty = false;
    bool m_syncing = false;
};

}  // namespace moyu
