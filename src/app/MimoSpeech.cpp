#include "app/MimoSpeech.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

namespace moyu {
namespace {

constexpr int kTimeoutMs = 60000;

QString serverMessage(const QByteArray& body)
{
    const QJsonObject error = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject();
    return error.value(QStringLiteral("message")).toString();
}

}  // namespace

QByteArray mimoRequestBody(const MimoConfig& config, const QString& text)
{
    QJsonArray messages;
    const QString style = config.style.trimmed();
    if (!style.isEmpty()) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), style}});
    }
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), text}});
    const QJsonObject audio{{QStringLiteral("format"), QStringLiteral("wav")}, {QStringLiteral("voice"), config.voice}};
    const QJsonObject body{
        {QStringLiteral("model"), config.model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("audio"), audio},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

SpeechResult parseMimoReply(int httpStatus, const QByteArray& body, const QString& networkError)
{
    SpeechResult result;
    if (httpStatus == 0) {
        result.failure = SpeechFailure::Transient;
        result.message = networkError.isEmpty() ? QStringLiteral("连不上 MiMo") : QStringLiteral("连不上 MiMo：%1").arg(networkError);
        return result;
    }
    if (httpStatus == 401 || httpStatus == 403) {
        result.failure = SpeechFailure::Auth;
        result.message = QStringLiteral("MiMo API Key 无效或没有权限");
        return result;
    }
    if (httpStatus == 429) {
        result.failure = SpeechFailure::Transient;
        result.message = QStringLiteral("MiMo 请求太频繁或额度用完了");
        return result;
    }
    if (httpStatus >= 500) {
        result.failure = SpeechFailure::Transient;
        result.message = QStringLiteral("MiMo 服务暂时出错（HTTP %1）").arg(httpStatus);
        return result;
    }
    if (httpStatus != 200) {
        result.failure = SpeechFailure::Fatal;
        const QString detail = serverMessage(body);
        result.message = detail.isEmpty() ? QStringLiteral("MiMo 返回 HTTP %1").arg(httpStatus)
                                          : QStringLiteral("MiMo 返回 HTTP %1：%2").arg(httpStatus).arg(detail);
        return result;
    }

    const QJsonArray choices = QJsonDocument::fromJson(body).object().value(QStringLiteral("choices")).toArray();
    const QJsonObject choice = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    if (choice.value(QStringLiteral("finish_reason")).toString() == QLatin1String("content_filter")) {
        result.failure = SpeechFailure::Rejected;
        result.message = QStringLiteral("这段被 MiMo 的内容审核拦下了");
        return result;
    }
    const QString data = choice.value(QStringLiteral("message"))
                             .toObject()
                             .value(QStringLiteral("audio"))
                             .toObject()
                             .value(QStringLiteral("data"))
                             .toString();
    result.audio = QByteArray::fromBase64(data.toLatin1());
    if (result.audio.isEmpty()) {
        result.failure = SpeechFailure::Fatal;
        result.message = QStringLiteral("MiMo 的返回里没有音频");
    }
    return result;
}

MimoSpeechEngine::MimoSpeechEngine(QObject* parent)
    : SpeechEngine(parent)
    , m_network(new QNetworkAccessManager(this))
{
    m_network->setProxy(QNetworkProxy::NoProxy);
}

MimoSpeechEngine::~MimoSpeechEngine()
{
    cancelAll();
}

quint64 MimoSpeechEngine::synthesize(const QString& text)
{
    const quint64 id = ++m_nextId;
    if (m_config.apiKey.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, [this, id] {
            SpeechResult result;
            result.failure = SpeechFailure::Auth;
            result.message = QStringLiteral("还没有填写 MiMo API Key");
            Q_EMIT finished(id, result);
        });
        return id;
    }
    QNetworkRequest request{QUrl(m_config.endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.trimmed().toUtf8());
    request.setTransferTimeout(kTimeoutMs);
    QNetworkReply* reply = m_network->post(request, mimoRequestBody(m_config, text));
    m_pending[id] = reply;
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] {
        reply->deleteLater();
        // cancelAll 已经把它摘掉了，abort 触发的这次 finished 不往外报。
        if (m_pending.erase(id) == 0) {
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        Q_EMIT finished(id, parseMimoReply(status, reply->readAll(), networkError));
    });
    return id;
}

void MimoSpeechEngine::cancelAll()
{
    auto pending = std::exchange(m_pending, {});
    for (auto& [id, reply] : pending) {
        reply->abort();
    }
}

}  // namespace moyu
