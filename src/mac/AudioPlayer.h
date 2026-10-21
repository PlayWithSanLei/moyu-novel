#pragma once

#include "app/SpeechEngine.h"

#include <memory>

namespace moyu {

// AVAudioPlayer 播一段完整的 WAV。变速保持音高。
class MacAudioOutput : public AudioOutput {
    Q_OBJECT

public:
    explicit MacAudioOutput(QObject* parent = nullptr);
    ~MacAudioOutput() override;

    bool play(const QByteArray& wav) override;
    void setRate(double rate) override;
    void pause() override;
    void resume() override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    double m_rate = 1.0;
};

}  // namespace moyu
