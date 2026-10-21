#pragma once

#include "app/SpeechEngine.h"
#include "core/SpeechText.h"

#include <QObject>

#include <deque>

namespace moyu {

enum class ReadAloudState { Idle, Playing, Paused };

// 一段一段读：播放当前段的同时合成下一段。读到本章最后几段时要下一章。
// 失败处理：
// - 内容审核拦下：同一段再试几次，还不行就跳过。
// - 断网、限流、5xx：退避重试，连续失败就暂停，恢复后从这一段接着读。
// - Key 无效：直接停。
class ReadAloud : public QObject {
    Q_OBJECT

public:
    static constexpr int kLookahead = 2;
    static constexpr int kRejectedAttempts = 3;
    static constexpr int kTransientAttempts = 4;

    ReadAloud(SpeechEngine* engine, AudioOutput* output, QObject* parent = nullptr);

    void start(const ChapterContent& content, quint32 byteOffset, quint32 chapterCount);
    // 只收正在等的那一章，别的直接忽略。可以放心把所有章节加载信号都接过来。
    void offerChapter(const ChapterContent& content);
    void pause();
    void resume();
    void stop();
    void setRate(double rate);

    ReadAloudState state() const { return m_state; }
    // 在读但还没声音：第一段、下一段没合成完、正在重试。
    bool buffering() const { return m_buffering; }
    bool waitingForChapter() const { return m_chapterRequested; }
    quint32 nextChapter() const { return m_nextChapter; }
    int queuedSegments() const { return static_cast<int>(m_queue.size()); }
    // 测试用：退避等待的毫秒数会乘这个系数。
    void setRetryScale(double scale) { m_retryScale = scale; }

Q_SIGNALS:
    void segmentStarted(quint32 chapter, quint32 byteStart, quint32 byteEnd);
    void chapterNeeded(quint32 index);
    void stateChanged(moyu::ReadAloudState state);
    void bufferingChanged(bool buffering);
    void notice(QString message);

private:
    // Stalled：重试次数用完了。等它排到最前面再暂停，正在播的那段让它播完。
    enum class Status { Fresh, Running, Backoff, Ready, Skipped, Stalled };
    struct Item {
        SpeechSegment segment;
        quint64 serial = 0;
        quint64 request = 0;
        QByteArray audio;
        Status status = Status::Fresh;
        int attempts = 0;
    };

    void enqueue(const ChapterContent& content, quint32 fromByte);
    void pump();
    void pumpOnce();
    void request(Item& item);
    void retryLater(Item& item, int delayMs);
    Item* findRequest(quint64 request);
    void onResult(quint64 request, const SpeechResult& result);
    void onPlaybackFinished();
    void setState(ReadAloudState state);
    void updateBuffering();

    SpeechEngine* m_engine = nullptr;
    AudioOutput* m_output = nullptr;
    ReadAloudState m_state = ReadAloudState::Idle;
    std::deque<Item> m_queue;
    bool m_playingFront = false;
    quint32 m_nextChapter = 0;
    quint32 m_chapterCount = 0;
    bool m_chapterRequested = false;
    quint64 m_serial = 0;
    // stop/start 换代，旧的重试定时器看到代号不对就不动。
    quint64 m_generation = 0;
    bool m_pumping = false;
    bool m_pumpAgain = false;
    double m_retryScale = 1.0;
    QString m_stallMessage;
    bool m_buffering = false;
};

}  // namespace moyu
