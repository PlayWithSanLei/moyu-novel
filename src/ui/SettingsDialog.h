#pragma once

#include "app/Hotkeys.h"
#include "app/SpeechSettings.h"
#include "ui/ReadingStyle.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace moyu {

enum class PreviewState { Idle, Synthesizing, Playing };

// 非模态。滑块拖动时立刻把样式交出去，调用方再决定穿透能不能开。
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;

    void setReadingStyle(const ReadingStyle& style);
    void setClickThrough(bool on);
    void setHideFromDock(bool on);
    void setHideOnBlur(bool on);
    void setHotkey(HotkeyRole role, const HotkeyChord& chord);
    void setChapterPattern(const QString& pattern);
    void setSpeechSettings(const SpeechSettings& settings);
    // message 非空时显示在试听按钮旁边，比如失败原因。
    void setPreviewState(PreviewState state, const QString& message = {});

Q_SIGNALS:
    void appearanceEdited(ReadingStyle style);
    void chapterPatternEdited(QString pattern);
    void regenerateRequested();
    void clickThroughRequested(bool on);
    void hideDockRequested(bool on);
    void hideOnBlurChanged(bool on);
    void hotkeyEdited(int role, HotkeyChord chord);
    void hotkeyRejected(QString message);
    void speechEdited(moyu::SpeechSettings settings);
    void speechPreviewRequested(QString text);
    void speechPreviewStopRequested();

protected:
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ReadingStyle collect() const;
    void emitAppearance();
    void stopRecording();
    void fillPresets();
    void syncPresetCombo();
    void updateHotkeyButton(HotkeyRole role);
    void beginRecord(HotkeyRole role);
    QWidget* buildSpeechPage();
    void emitSpeech();
    void updateMarkColorButton();

    ReadingStyle m_style;
    bool m_clickThrough = false;
    bool m_hideFromDock = false;
    bool m_hideOnBlur = false;
    bool m_applying = false;
    bool m_recording = false;
    HotkeyRole m_recordRole = HotkeyRole::Boss;
    HotkeyChord m_chords[4];

    QComboBox* m_preset = nullptr;
    QSlider* m_bgOpacity = nullptr;
    QSlider* m_textOpacity = nullptr;
    QSlider* m_lineSpacing = nullptr;
    QSlider* m_paragraphGap = nullptr;
    QSlider* m_margin = nullptr;
    QFontComboBox* m_font = nullptr;
    QSpinBox* m_fontSize = nullptr;
    QCheckBox* m_clickThroughBox = nullptr;
    QCheckBox* m_hideDockBox = nullptr;
    QCheckBox* m_hideOnBlurBox = nullptr;
    QPushButton* m_bgColor = nullptr;
    QPushButton* m_textColor = nullptr;
    QPushButton* m_hotkeyButtons[4] = {};
    QLineEdit* m_chapterPattern = nullptr;

    SpeechSettings m_speech;
    QLineEdit* m_apiKey = nullptr;
    QComboBox* m_voice = nullptr;
    QLineEdit* m_speechStyle = nullptr;
    QSlider* m_speechRate = nullptr;
    QCheckBox* m_speechMark = nullptr;
    QPushButton* m_markColor = nullptr;
    QPlainTextEdit* m_previewText = nullptr;
    QPushButton* m_previewButton = nullptr;
    QLabel* m_previewStatus = nullptr;
    PreviewState m_previewState = PreviewState::Idle;
};

}  // namespace moyu
