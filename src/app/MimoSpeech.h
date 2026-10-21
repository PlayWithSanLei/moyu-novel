#pragma once

#include "app/SpeechEngine.h"

#include <QString>

#include <unordered_map>

class QNetworkAccessManager;
class QNetworkReply;

namespace moyu {

struct MimoConfig {
    QString apiKey;
    QString voice = QStringLiteral("茉莉");
    QString style;
    QString model = QStringLiteral("mimo-v2.5-tts");
    QString endpoint = QStringLiteral("https://api.xiaomimimo.com/v1/chat/completions");
};

// 要读的文字放在 assistant 消息里；user 消息是语气描述，不会被读出来。
QByteArray mimoRequestBody(const MimoConfig& config, const QString& text);
// httpStatus 为 0 表示没拿到 HTTP 响应（断网、超时、TLS 失败）。
SpeechResult parseMimoReply(int httpStatus, const QByteArray& body, const QString& networkError);

// 小米 MiMo 语音合成。非流式、WAV 输出，一段一个请求。直连，不走代理。
class MimoSpeechEngine : public SpeechEngine {
    Q_OBJECT

public:
    explicit MimoSpeechEngine(QObject* parent = nullptr);
    ~MimoSpeechEngine() override;

    void setConfig(const MimoConfig& config) { m_config = config; }
    const MimoConfig& config() const { return m_config; }

    quint64 synthesize(const QString& text) override;
    void cancelAll() override;

private:
    MimoConfig m_config;
    QNetworkAccessManager* m_network = nullptr;
    std::unordered_map<quint64, QNetworkReply*> m_pending;
    quint64 m_nextId = 0;
};

}  // namespace moyu
