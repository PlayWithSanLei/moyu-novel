#include "app/ReadAloud.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace moyu {
namespace {

constexpr int kRejectedRetryMs = 500;
constexpr int kTransientBaseMs = 1000;

}  // namespace

ReadAloud::ReadAloud(SpeechEngine* engine, AudioOutput* output, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_output(output)
{
    connect(m_engine, &SpeechEngine::finished, this, &ReadAloud::onResult);
    connect(m_output, &AudioOutput::finished, this, &ReadAloud::onPlaybackFinished);
}

void ReadAloud::start(const ChapterContent& content, quint32 byteOffset, quint32 chapterCount)
{
    stop();
    m_chapterCount = chapterCount;
    m_nextChapter = content.index + 1;
    enqueue(content, byteOffset);
    setState(ReadAloudState::Playing);
    pump();
}

void ReadAloud::offerChapter(const ChapterContent& content)
{
    if (m_state == ReadAloudState::Idle || !m_chapterRequested || content.index != m_nextChapter) {
        return;
    }
    m_chapterRequested = false;
    ++m_nextChapter;
    enqueue(content, 0);
    pump();
}

void ReadAloud::pause()
{
    if (m_state != ReadAloudState::Playing) {
        return;
    }
    if (m_playingFront) {
        m_output->pause();
    }
    setState(ReadAloudState::Paused);
}

void ReadAloud::resume()
{
    if (m_state != ReadAloudState::Paused) {
        return;
    }
    setState(ReadAloudState::Playing);
    if (m_playingFront) {
        m_output->resume();
    }
    pump();
}

void ReadAloud::stop()
{
    ++m_generation;
    m_engine->cancelAll();
    if (m_playingFront) {
        m_output->stop();
    }
    m_playingFront = false;
    m_queue.clear();
    m_chapterRequested = false;
    setState(ReadAloudState::Idle);
}

void ReadAloud::setRate(double rate)
{
    m_output->setRate(rate);
}

void ReadAloud::enqueue(const ChapterContent& content, quint32 fromByte)
{
    for (SpeechSegment& segment : speechSegments(content, fromByte)) {
        Item item;
        item.segment = std::move(segment);
        item.serial = ++m_serial;
        m_queue.push_back(std::move(item));
    }
}

void ReadAloud::pump()
{
    // 槽里可能同步回调 offerChapter，再进来一次。只记一笔，外层循环接着跑。
    if (m_pumping) {
        m_pumpAgain = true;
        return;
    }
    m_pumping = true;
    do {
        m_pumpAgain = false;
        pumpOnce();
    } while (m_pumpAgain && m_state != ReadAloudState::Idle);
    m_pumping = false;
    updateBuffering();
}

void ReadAloud::pumpOnce()
{
    if (m_state == ReadAloudState::Idle) {
        return;
    }
    while (!m_playingFront && !m_queue.empty() && m_queue.front().status == Status::Skipped) {
        m_queue.pop_front();
    }

    if (!m_playingFront && m_state == ReadAloudState::Playing && !m_queue.empty() &&
        m_queue.front().status == Status::Stalled) {
        // 暂停在这一段。按继续时从头重试。
        m_queue.front().status = Status::Fresh;
        pause();
        Q_EMIT notice(QStringLiteral("朗读已暂停：%1。好了以后按 Command+R 继续").arg(m_stallMessage));
        return;
    }

    int live = 0;
    for (Item& item : m_queue) {
        if (item.status == Status::Skipped) {
            continue;
        }
        if (live >= kLookahead) {
            break;
        }
        if (item.status == Status::Fresh) {
            request(item);
        }
        ++live;
    }

    if (!m_chapterRequested && m_nextChapter < m_chapterCount && live < kLookahead) {
        m_chapterRequested = true;
        Q_EMIT chapterNeeded(m_nextChapter);
        if (m_state == ReadAloudState::Idle) {
            return;
        }
    }

    if (!m_playingFront && m_state == ReadAloudState::Playing && !m_queue.empty() &&
        m_queue.front().status == Status::Ready) {
        Item& front = m_queue.front();
        if (m_output->play(front.audio)) {
            m_playingFront = true;
            Q_EMIT segmentStarted(front.segment.chapter, front.segment.byteStart, front.segment.byteEnd);
        } else {
            front.status = Status::Skipped;
            Q_EMIT notice(QStringLiteral("有一段音频播放不了，已跳过"));
            m_pumpAgain = true;
        }
        return;
    }

    if (m_queue.empty() && !m_chapterRequested && m_nextChapter >= m_chapterCount) {
        stop();
        Q_EMIT notice(QStringLiteral("全书读完了"));
    }
}

void ReadAloud::request(Item& item)
{
    item.status = Status::Running;
    item.request = m_engine->synthesize(item.segment.text);
}

void ReadAloud::retryLater(Item& item, int delayMs)
{
    item.status = Status::Backoff;
    item.request = 0;
    const quint64 generation = m_generation;
    const quint64 serial = item.serial;
    const int scaled = std::max(0, static_cast<int>(std::lround(delayMs * m_retryScale)));
    QTimer::singleShot(scaled, this, [this, generation, serial] {
        if (generation != m_generation) {
            return;
        }
        for (Item& candidate : m_queue) {
            if (candidate.serial == serial && candidate.status == Status::Backoff) {
                candidate.status = Status::Fresh;
                pump();
                return;
            }
        }
    });
}

ReadAloud::Item* ReadAloud::findRequest(quint64 request)
{
    for (Item& item : m_queue) {
        if (item.status == Status::Running && item.request == request) {
            return &item;
        }
    }
    return nullptr;
}

void ReadAloud::onResult(quint64 request, const SpeechResult& result)
{
    Item* item = findRequest(request);
    if (item == nullptr) {
        return;
    }
    switch (result.failure) {
    case SpeechFailure::None:
        item->status = Status::Ready;
        item->audio = result.audio;
        break;
    case SpeechFailure::Rejected:
        ++item->attempts;
        if (item->attempts < kRejectedAttempts) {
            retryLater(*item, kRejectedRetryMs);
        } else {
            item->status = Status::Skipped;
            Q_EMIT notice(QStringLiteral("有一段没通过 MiMo 的内容审核，已跳过"));
        }
        break;
    case SpeechFailure::Transient:
        ++item->attempts;
        if (item->attempts < kTransientAttempts) {
            retryLater(*item, kTransientBaseMs << (item->attempts - 1));
        } else {
            item->status = Status::Stalled;
            item->attempts = 0;
            m_stallMessage = result.message;
        }
        break;
    case SpeechFailure::Auth:
        stop();
        Q_EMIT notice(QStringLiteral("朗读已停止：%1，请到 设置 → 听书 检查").arg(result.message));
        return;
    case SpeechFailure::Fatal:
        item->status = Status::Skipped;
        Q_EMIT notice(QStringLiteral("有一段合成失败，已跳过：%1").arg(result.message));
        break;
    }
    pump();
}

void ReadAloud::onPlaybackFinished()
{
    if (!m_playingFront) {
        return;
    }
    m_playingFront = false;
    if (!m_queue.empty()) {
        m_queue.pop_front();
    }
    pump();
}

void ReadAloud::setState(ReadAloudState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(state);
    updateBuffering();
}

void ReadAloud::updateBuffering()
{
    const bool buffering = m_state == ReadAloudState::Playing && !m_playingFront;
    if (buffering == m_buffering) {
        return;
    }
    m_buffering = buffering;
    Q_EMIT bufferingChanged(buffering);
}

}  // namespace moyu
