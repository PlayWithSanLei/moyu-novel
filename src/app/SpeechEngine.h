#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>

namespace moyu {

enum class SpeechFailure {
    None,
    // 内容审核拦下。同一句重试有时能过。
    Rejected,
    // 断网、超时、限流、服务端 5xx。等一会儿再试。
    Transient,
    // Key 无效或没有权限。重试没用。
    Auth,
    // 请求本身有问题。跳过这一句。
    Fatal,
};

struct SpeechResult {
    SpeechFailure failure = SpeechFailure::None;
    // 完整的 WAV 文件。
    QByteArray audio;
    QString message;
};

// 合成引擎。换本地模型时只换这一层。
class SpeechEngine : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // 返回请求号。结果从 finished 异步回来，每个请求号最多回一次；cancelAll 之后不再回。
    virtual quint64 synthesize(const QString& text) = 0;
    virtual void cancelAll() = 0;

Q_SIGNALS:
    void finished(quint64 request, moyu::SpeechResult result);
};

class AudioOutput : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool play(const QByteArray& wav) = 0;
    // 1.0 是原速。
    virtual void setRate(double rate) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;

Q_SIGNALS:
    // 自然播完才发，stop() 不发。
    void finished();
};

}  // namespace moyu

Q_DECLARE_METATYPE(moyu::SpeechResult)
