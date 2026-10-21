#include "app/SpeechSettings.h"

#include <algorithm>

namespace moyu {
namespace {

const QString kKeyApi = QStringLiteral("speech/apiKey");
const QString kKeyVoice = QStringLiteral("speech/voice");
const QString kKeyStyle = QStringLiteral("speech/style");
const QString kKeyRate = QStringLiteral("speech/rate");
const QString kKeyMark = QStringLiteral("speech/mark");
const QString kKeyMarkColor = QStringLiteral("speech/markColor");

}  // namespace

std::vector<QString> speechVoices()
{
    return {QStringLiteral("茉莉"), QStringLiteral("冰糖"), QStringLiteral("苏打"), QStringLiteral("白桦")};
}

std::vector<QString> speechSettingKeys()
{
    return {kKeyApi, kKeyVoice, kKeyStyle, kKeyRate, kKeyMark, kKeyMarkColor};
}

std::vector<std::pair<QString, QString>> speechSettingsItems(const SpeechSettings& settings)
{
    return {
        {kKeyApi, settings.apiKey},
        {kKeyVoice, settings.voice},
        {kKeyStyle, settings.style},
        {kKeyRate, QString::number(settings.ratePercent)},
        {kKeyMark, settings.mark ? QStringLiteral("1") : QStringLiteral("0")},
        {kKeyMarkColor, settings.markColor.name(QColor::HexArgb)},
    };
}

SpeechSettings speechSettingsFromItems(const std::vector<std::pair<QString, QString>>& items, SpeechSettings base)
{
    const auto voices = speechVoices();
    for (const auto& [key, value] : items) {
        if (key == kKeyApi) {
            base.apiKey = value.trimmed();
        } else if (key == kKeyVoice) {
            if (std::find(voices.begin(), voices.end(), value) != voices.end()) {
                base.voice = value;
            }
        } else if (key == kKeyStyle) {
            base.style = value;
        } else if (key == kKeyRate) {
            bool ok = false;
            const int rate = value.toInt(&ok);
            if (ok) {
                base.ratePercent = std::clamp(rate, 50, 200);
            }
        } else if (key == kKeyMark) {
            if (value == QLatin1String("1")) {
                base.mark = true;
            } else if (value == QLatin1String("0")) {
                base.mark = false;
            }
        } else if (key == kKeyMarkColor) {
            const QColor color = QColor::fromString(value);
            if (color.isValid()) {
                base.markColor = color;
            }
        }
    }
    return base;
}

}  // namespace moyu
