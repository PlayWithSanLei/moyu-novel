#pragma once

#include <QColor>
#include <QString>

#include <utility>
#include <vector>

namespace moyu {

inline constexpr const char* kDefaultSpeechStyle = "用自然的有声书朗读方式，语速适中，对话部分带一点角色语气";

struct SpeechSettings {
    // 存在本地设置库里，不打日志。
    QString apiKey;
    QString voice = QStringLiteral("茉莉");
    // 空表示用 kDefaultSpeechStyle。
    QString style;
    int ratePercent = 100;
    // 跟读标记默认关。摸鱼用，太显眼不好。
    bool mark = false;
    QColor markColor{255, 196, 60, 50};

    QString effectiveStyle() const
    {
        const QString trimmed = style.trimmed();
        return trimmed.isEmpty() ? QString::fromUtf8(kDefaultSpeechStyle) : trimmed;
    }
};

// MiMo 预置的中文音色。
std::vector<QString> speechVoices();

std::vector<std::pair<QString, QString>> speechSettingsItems(const SpeechSettings& settings);
SpeechSettings speechSettingsFromItems(const std::vector<std::pair<QString, QString>>& items, SpeechSettings base);
std::vector<QString> speechSettingKeys();

}  // namespace moyu
