#include "ui/SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
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
    resize(420, 560);
    for (HotkeyRole role : kRoles) {
        m_chords[roleIndex(role)] = defaultChord(role);
    }

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    layout->addLayout(form);

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
