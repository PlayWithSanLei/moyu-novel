#pragma once

#include "app/Hotkeys.h"
#include "app/SpeechSettings.h"
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
class MacAudioOutput;
class MimoSpeechEngine;
class ReadAloud;
class MainWindow;
class SettingsDialog;
enum class PreviewState;
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
    void wireSpeech();
    void applySpeechSettings();
    void toggleSpeech();
    void startSpeechAt(quint32 chapter, quint32 byteOffset);
    void stopSpeech();
    void previewSpeech(const QString& text);
    void stopPreview();
    void showPreviewState(PreviewState state, const QString& message = {});
    void syncSpeechActions();
    void syncSpeechStatus();

    std::unique_ptr<Database> m_database;
    std::unique_ptr<BookSession> m_session;
    std::unique_ptr<MainWindow> m_window;
    QAction* m_pin = nullptr;
    QAction* m_clickAction = nullptr;
    QAction* m_dockAction = nullptr;
    QAction* m_pageAction = nullptr;
    QAction* m_autoAction = nullptr;
    QAction* m_speakAction = nullptr;
    QAction* m_stopSpeakAction = nullptr;
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

    SpeechSettings m_speechSettings;
    MimoSpeechEngine* m_mimo = nullptr;
    MacAudioOutput* m_audio = nullptr;
    ReadAloud* m_speech = nullptr;
    // 试听另起一个引擎，不和朗读队列抢请求号。
    MimoSpeechEngine* m_previewEngine = nullptr;
    quint64 m_previewRequest = 0;
    bool m_previewPlaying = false;
    bool m_resumeSpeechAfterBoss = false;
};

}  // namespace moyu
