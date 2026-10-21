#include "ui/SettingsDialog.h"

#include "core/SpeechText.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace moyu {
namespace {

constexpr HotkeyRole kRoles[] = {
    HotkeyRole::Boss,
    HotkeyRole::NextPage,
    HotkeyRole::PreviousPage,
    HotkeyRole::ToggleClickThrough,
};

const char* roleTitle(HotkeyRole role)
{
    switch (role) {
    case HotkeyRole::Boss:
        return "老板键";
    case HotkeyRole::NextPage:
        return "下一页";
    case HotkeyRole::PreviousPage:
        return "上一页";
    case HotkeyRole::ToggleClickThrough:
        return "切换穿透";
    }
    return "";
}

int roleIndex(HotkeyRole role)
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

QSlider* makeSlider(int minValue, int maxValue, int value, QWidget* parent)
{
    auto* slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(minValue, maxValue);
    slider->setValue(value);
    return slider;
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("设置"));
    setModal(false);
    resize(440, 600);
    for (HotkeyRole role : kRoles) {
        m_chords[roleIndex(role)] = defaultChord(role);
    }

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);
    auto* readingPage = new QWidget(tabs);
    auto* form = new QFormLayout(readingPage);
    tabs->addTab(readingPage, QStringLiteral("阅读"));
    tabs->addTab(buildSpeechPage(), QStringLiteral("听书"));

    m_preset = new QComboBox(this);
    fillPresets();
    form->addRow(QStringLiteral("背景"), m_preset);

    auto* colors = new QHBoxLayout();
    m_bgColor = new QPushButton(QStringLiteral("背景色"), this);
    m_textColor = new QPushButton(QStringLiteral("文字色"), this);
    colors->addWidget(m_bgColor);
    colors->addWidget(m_textColor);
    form->addRow(QStringLiteral("自定义"), colors);

    m_bgOpacity = makeSlider(0, 100, m_style.backgroundOpacityPercent, this);
    form->addRow(QStringLiteral("背景透明度"), m_bgOpacity);
    m_textOpacity = makeSlider(10, 100, m_style.textOpacityPercent, this);
    form->addRow(QStringLiteral("文字透明度"), m_textOpacity);

    m_font = new QFontComboBox(this);
    m_fontSize = new QSpinBox(this);
    m_fontSize->setRange(8, 72);
    m_fontSize->setValue(m_style.fontPointSize);
    auto* fontRow = new QHBoxLayout();
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_fontSize);
    form->addRow(QStringLiteral("字体"), fontRow);

    m_lineSpacing = makeSlider(80, 200, m_style.lineSpacingPercent, this);
    form->addRow(QStringLiteral("行距 %"), m_lineSpacing);
    m_paragraphGap = makeSlider(0, 48, m_style.paragraphGap, this);
    form->addRow(QStringLiteral("段距"), m_paragraphGap);
    m_margin = makeSlider(8, 64, m_style.margin, this);
    form->addRow(QStringLiteral("页边距"), m_margin);

    m_clickThroughBox = new QCheckBox(QStringLiteral("鼠标穿透"), this);
    m_hideDockBox = new QCheckBox(QStringLiteral("不在 Dock 显示"), this);
    m_hideOnBlurBox = new QCheckBox(QStringLiteral("失去焦点时隐藏"), this);
    form->addRow(QString(), m_clickThroughBox);
    form->addRow(QString(), m_hideDockBox);
    form->addRow(QString(), m_hideOnBlurBox);

    for (HotkeyRole role : kRoles) {
        auto* button = new QPushButton(chordLabel(m_chords[roleIndex(role)]), this);
        m_hotkeyButtons[roleIndex(role)] = button;
        form->addRow(QString::fromUtf8(roleTitle(role)), button);
        connect(button, &QPushButton::clicked, this, [this, role] { beginRecord(role); });
    }

    m_chapterPattern = new QLineEdit(this);
    m_chapterPattern->setPlaceholderText(QStringLiteral("留空则用内置规则，例如 ^第.+章"));
    auto* regenerate = new QPushButton(QStringLiteral("重新生成目录"), this);
    auto* regexRow = new QHBoxLayout();
    regexRow->addWidget(m_chapterPattern, 1);
    regexRow->addWidget(regenerate);
    form->addRow(QStringLiteral("章节正则"), regexRow);
    connect(m_chapterPattern, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_applying) {
            Q_EMIT chapterPatternEdited(text);
        }
    });
    connect(regenerate, &QPushButton::clicked, this, [this] { Q_EMIT regenerateRequested(); });

    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    layout->addWidget(close);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    const auto edited = [this] { emitAppearance(); };
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_applying || index < 0) {
            return;
        }
        const auto preset = static_cast<PagePreset>(m_preset->itemData(index).toInt());
        m_style = presetStyle(preset, m_style);
        setReadingStyle(m_style);
        emitAppearance();
    });
    connect(m_bgOpacity, &QSlider::valueChanged, this, edited);
    connect(m_textOpacity, &QSlider::valueChanged, this, edited);
    connect(m_lineSpacing, &QSlider::valueChanged, this, edited);
    connect(m_paragraphGap, &QSlider::valueChanged, this, edited);
    connect(m_margin, &QSlider::valueChanged, this, edited);
    connect(m_fontSize, &QSpinBox::valueChanged, this, edited);
    connect(m_font, &QFontComboBox::currentFontChanged, this, edited);
    connect(m_bgColor, &QPushButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(m_style.background, this, QStringLiteral("背景色"));
        if (!picked.isValid()) {
            return;
        }
        m_style.background = QColor(picked.red(), picked.green(), picked.blue());
        m_style.preset = PagePreset::Custom;
        syncPresetCombo();
        emitAppearance();
    });
    connect(m_textColor, &QPushButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(m_style.text, this, QStringLiteral("文字色"));
        if (!picked.isValid()) {
            return;
        }
        m_style.text = QColor(picked.red(), picked.green(), picked.blue());
        m_style.preset = PagePreset::Custom;
        syncPresetCombo();
        emitAppearance();
    });
    connect(m_clickThroughBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_applying) {
            Q_EMIT clickThroughRequested(on);
        }
    });
    connect(m_hideDockBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_applying) {
            Q_EMIT hideDockRequested(on);
        }
    });
    connect(m_hideOnBlurBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_applying) {
            m_hideOnBlur = on;
            Q_EMIT hideOnBlurChanged(on);
        }
    });
}

SettingsDialog::~SettingsDialog()
{
    stopRecording();
}

void SettingsDialog::hideEvent(QHideEvent* event)
{
    stopRecording();
    QDialog::hideEvent(event);
}

void SettingsDialog::stopRecording()
{
    if (!m_recording) {
        return;
    }
    m_recording = false;
    if (qApp != nullptr) {
        qApp->removeEventFilter(this);
    }
    updateHotkeyButton(m_recordRole);
}

void SettingsDialog::fillPresets()
{
    const struct {
        PagePreset preset;
        const char* label;
    } rows[] = {
        {PagePreset::Dark, "深色"},
        {PagePreset::White, "白色"},
        {PagePreset::Green, "护眼绿"},
        {PagePreset::Paper, "羊皮纸"},
        {PagePreset::Custom, "自定义"},
    };
    for (const auto& row : rows) {
        m_preset->addItem(QString::fromUtf8(row.label), static_cast<int>(row.preset));
    }
}

void SettingsDialog::syncPresetCombo()
{
    QSignalBlocker blocker(m_preset);
    const int index = m_preset->findData(static_cast<int>(m_style.preset));
    if (index >= 0) {
        m_preset->setCurrentIndex(index);
    }
}

void SettingsDialog::setReadingStyle(const ReadingStyle& style)
{
    m_applying = true;
    m_style = style;
    syncPresetCombo();
    m_bgOpacity->setValue(style.backgroundOpacityPercent);
    m_textOpacity->setValue(style.textOpacityPercent);
    m_lineSpacing->setValue(style.lineSpacingPercent);
    m_paragraphGap->setValue(style.paragraphGap);
    m_margin->setValue(style.margin);
    m_fontSize->setValue(style.fontPointSize);
    if (!style.fontFamily.isEmpty()) {
        m_font->setCurrentFont(QFont(style.fontFamily));
    }
    m_applying = false;
}

void SettingsDialog::setClickThrough(bool on)
{
    m_applying = true;
    m_clickThrough = on;
    m_clickThroughBox->setChecked(on);
    m_applying = false;
}

void SettingsDialog::setHideFromDock(bool on)
{
    m_applying = true;
    m_hideFromDock = on;
    m_hideDockBox->setChecked(on);
    m_applying = false;
}

void SettingsDialog::setHideOnBlur(bool on)
{
    m_applying = true;
    m_hideOnBlur = on;
    m_hideOnBlurBox->setChecked(on);
    m_applying = false;
}

void SettingsDialog::setChapterPattern(const QString& pattern)
{
    m_applying = true;
    m_chapterPattern->setText(pattern);
    m_applying = false;
}

void SettingsDialog::setHotkey(HotkeyRole role, const HotkeyChord& chord)
{
    m_chords[roleIndex(role)] = chord;
    updateHotkeyButton(role);
}

void SettingsDialog::updateHotkeyButton(HotkeyRole role)
{
    QPushButton* button = m_hotkeyButtons[roleIndex(role)];
    if (button == nullptr) {
        return;
    }
    if (m_recording && m_recordRole == role) {
        button->setText(QStringLiteral("请按组合键…"));
        return;
    }
    button->setText(chordLabel(m_chords[roleIndex(role)]));
}

void SettingsDialog::beginRecord(HotkeyRole role)
{
    if (m_recording) {
        qApp->removeEventFilter(this);
    }
    m_recording = true;
    m_recordRole = role;
    updateHotkeyButton(role);
    qApp->installEventFilter(this);
}

QWidget* SettingsDialog::buildSpeechPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    m_apiKey = new QLineEdit(page);
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(QStringLiteral("小米 MiMo 开放平台的 API Key"));
    auto* showKey = new QCheckBox(QStringLiteral("显示"), page);
    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(m_apiKey, 1);
    keyRow->addWidget(showKey);
    form->addRow(QStringLiteral("API Key"), keyRow);
    auto* keyHint = new QLabel(QStringLiteral("在 <a href=\"https://mimo.mi.com\">mimo.mi.com</a> 控制台申请。"
                                              "Key 只存在本机，朗读时正文会发给 MiMo 合成。"),
                               page);
    keyHint->setOpenExternalLinks(true);
    keyHint->setWordWrap(true);
    form->addRow(QString(), keyHint);

    m_voice = new QComboBox(page);
    for (const QString& voice : speechVoices()) {
        m_voice->addItem(voice);
    }
    form->addRow(QStringLiteral("音色"), m_voice);

    m_speechStyle = new QLineEdit(page);
    m_speechStyle->setPlaceholderText(QString::fromUtf8(kDefaultSpeechStyle));
    form->addRow(QStringLiteral("语气"), m_speechStyle);

    m_speechRate = makeSlider(50, 200, m_speech.ratePercent, page);
    m_speechRate->setSingleStep(10);
    m_speechRate->setPageStep(10);
    form->addRow(QStringLiteral("语速 %"), m_speechRate);

    m_speechMark = new QCheckBox(QStringLiteral("朗读时标出当前段"), page);
    m_markColor = new QPushButton(QStringLiteral("标记色"), page);
    auto* markRow = new QHBoxLayout();
    markRow->addWidget(m_speechMark, 1);
    markRow->addWidget(m_markColor);
    form->addRow(QString(), markRow);
    form->addRow(QString(), new QLabel(QStringLiteral("Command+R 朗读 / 暂停，Shift+Command+R 停止。"), page));

    // 用上面的音色、语气、语速读这段文字。
    m_previewText = new QPlainTextEdit(page);
    m_previewText->setPlainText(QStringLiteral("天色渐晚，他推开客栈的门。掌柜抬头看了他一眼，低声说：“客官，今晚怕是没有空房了。”"));
    m_previewText->setPlaceholderText(QStringLiteral("写一段想听的文字"));
    m_previewText->setFixedHeight(m_previewText->fontMetrics().lineSpacing() * 4 + 12);
    form->addRow(QStringLiteral("试听文字"), m_previewText);
    m_previewButton = new QPushButton(QStringLiteral("试听"), page);
    m_previewStatus = new QLabel(page);
    m_previewStatus->setWordWrap(true);
    auto* previewRow = new QHBoxLayout();
    previewRow->addWidget(m_previewButton);
    previewRow->addWidget(m_previewStatus, 1);
    form->addRow(QString(), previewRow);

    connect(showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_apiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(m_apiKey, &QLineEdit::textChanged, this, [this] { emitSpeech(); });
    connect(m_voice, &QComboBox::currentIndexChanged, this, [this] { emitSpeech(); });
    connect(m_speechStyle, &QLineEdit::textChanged, this, [this] { emitSpeech(); });
    connect(m_speechRate, &QSlider::valueChanged, this, [this] { emitSpeech(); });
    connect(m_speechMark, &QCheckBox::toggled, this, [this] { emitSpeech(); });
    connect(m_previewButton, &QPushButton::clicked, this, [this] {
        if (m_previewState != PreviewState::Idle) {
            Q_EMIT speechPreviewStopRequested();
            return;
        }
        const QString text = m_previewText->toPlainText().trimmed();
        if (text.isEmpty()) {
            setPreviewState(PreviewState::Idle, QStringLiteral("先写点要试听的文字"));
            return;
        }
        if (text.size() > kMaxSpeechSegmentChars) {
            setPreviewState(PreviewState::Idle, QStringLiteral("试听最多 %1 个字").arg(kMaxSpeechSegmentChars));
            return;
        }
        Q_EMIT speechPreviewRequested(text);
    });
    connect(m_markColor, &QPushButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(m_speech.markColor,
                                                     this,
                                                     QStringLiteral("标记色"),
                                                     QColorDialog::ShowAlphaChannel);
        if (!picked.isValid()) {
            return;
        }
        m_speech.markColor = picked;
        updateMarkColorButton();
        emitSpeech();
    });
    updateMarkColorButton();
    return page;
}

void SettingsDialog::setPreviewState(PreviewState state, const QString& message)
{
    m_previewState = state;
    m_previewButton->setText(state == PreviewState::Idle ? QStringLiteral("试听") : QStringLiteral("停止"));
    if (!message.isEmpty()) {
        m_previewStatus->setText(message);
        return;
    }
    switch (state) {
    case PreviewState::Idle:
        m_previewStatus->clear();
        break;
    case PreviewState::Synthesizing:
        m_previewStatus->setText(QStringLiteral("合成中…"));
        break;
    case PreviewState::Playing:
        m_previewStatus->setText(QStringLiteral("播放中"));
        break;
    }
}

void SettingsDialog::updateMarkColorButton()
{
    // 色块画在按钮左边，透明度也看得出来。
    QPixmap swatch(14, 14);
    swatch.fill(m_speech.markColor);
    m_markColor->setIcon(QIcon(swatch));
}

void SettingsDialog::setSpeechSettings(const SpeechSettings& settings)
{
    m_applying = true;
    m_speech = settings;
    m_apiKey->setText(settings.apiKey);
    const int voice = m_voice->findText(settings.voice);
    m_voice->setCurrentIndex(voice >= 0 ? voice : 0);
    m_speechStyle->setText(settings.style);
    m_speechRate->setValue(settings.ratePercent);
    m_speechMark->setChecked(settings.mark);
    updateMarkColorButton();
    m_applying = false;
}

void SettingsDialog::emitSpeech()
{
    if (m_applying) {
        return;
    }
    m_speech.apiKey = m_apiKey->text().trimmed();
    m_speech.voice = m_voice->currentText();
    m_speech.style = m_speechStyle->text();
    m_speech.ratePercent = m_speechRate->value();
    m_speech.mark = m_speechMark->isChecked();
    Q_EMIT speechEdited(m_speech);
}

ReadingStyle SettingsDialog::collect() const
{
    ReadingStyle style = m_style;
    style.backgroundOpacityPercent = m_bgOpacity->value();
    style.textOpacityPercent = m_textOpacity->value();
    style.lineSpacingPercent = m_lineSpacing->value();
    style.paragraphGap = m_paragraphGap->value();
    style.margin = m_margin->value();
    style.fontPointSize = m_fontSize->value();
    style.fontFamily = m_font->currentFont().family();
    return style;
}

void SettingsDialog::emitAppearance()
{
    if (m_applying) {
        return;
    }
    m_style = collect();
    Q_EMIT appearanceEdited(m_style);
}

bool SettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_recording || event->type() != QEvent::KeyPress) {
        return QDialog::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->isAutoRepeat()) {
        return true;
    }
    const int code = key->key();
    if (code == Qt::Key_Escape) {
        m_recording = false;
        qApp->removeEventFilter(this);
        updateHotkeyButton(m_recordRole);
        return true;
    }
    if (code == Qt::Key_Shift || code == Qt::Key_Control || code == Qt::Key_Alt || code == Qt::Key_Meta) {
        return true;
    }
    const auto modifiers = key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (modifiers == Qt::NoModifier) {
        Q_EMIT hotkeyRejected(QStringLiteral("请使用带修饰键的组合"));
        return true;
    }
    if (isReservedSystemHotkey(code, modifiers)) {
        Q_EMIT hotkeyRejected(QStringLiteral("不能录 Cmd+Q、Cmd+Tab 这类系统组合键"));
        return true;
    }
    const quint32 native = key->nativeVirtualKey();
    if (native == 0 || native > 0xFFFF) {
        Q_EMIT hotkeyRejected(QStringLiteral("没有拿到按键码，请直接在键盘上按"));
        return true;
    }
    HotkeyChord chord;
    chord.qtKey = code;
    chord.qtModifiers = static_cast<int>(modifiers);
    chord.keyCode = static_cast<unsigned short>(native);
    m_chords[roleIndex(m_recordRole)] = chord;
    m_recording = false;
    qApp->removeEventFilter(this);
    updateHotkeyButton(m_recordRole);
    Q_EMIT hotkeyEdited(static_cast<int>(m_recordRole), chord);
    return true;
}

}  // namespace moyu
