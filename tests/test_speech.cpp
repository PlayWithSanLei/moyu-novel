#include "app/MimoSpeech.h"
#include "app/ReadAloud.h"
#include "app/SpeechSettings.h"
#include "core/ChapterDetector.h"
#include "core/SpeechText.h"
#include "mac/AudioPlayer.h"
#include "reader/ReaderView.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"

#include <QApplication>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <deque>
#include <vector>

namespace {

moyu::ChapterContent chapterFrom(quint32 index, const QString& text, bool heading = true)
{
    const QByteArray utf8 = text.toUtf8();
    moyu::ChapterIndex chapter;
    chapter.index = index;
    chapter.title = heading ? QStringLiteral("第%1章").arg(index + 1) : QStringLiteral("正文");
    chapter.byteEnd = static_cast<quint64>(utf8.size());
    return moyu::makeChapterContent(chapter, utf8.constData(), static_cast<quint64>(utf8.size()));
}

// 合成结果由测试手动放出来，能控制先后和失败类型。
class FakeEngine : public moyu::SpeechEngine {
public:
    struct Call {
        quint64 id = 0;
        QString text;
    };

    quint64 synthesize(const QString& text) override
    {
        const quint64 id = ++m_next;
        calls.push_back({id, text});
        pending.push_back({id, text});
        return id;
    }

    void cancelAll() override
    {
        pending.clear();
        ++cancels;
    }

    // 放出最早的一个未完成请求。
    QString finishOldest(moyu::SpeechFailure failure = moyu::SpeechFailure::None)
    {
        if (pending.empty()) {
            return {};
        }
        const Call call = pending.front();
        pending.pop_front();
        moyu::SpeechResult result;
        result.failure = failure;
        result.message = QStringLiteral("测试失败");
        if (failure == moyu::SpeechFailure::None) {
            result.audio = call.text.toUtf8();
        }
        Q_EMIT finished(call.id, result);
        return call.text;
    }

    std::vector<Call> calls;
    std::deque<Call> pending;
    int cancels = 0;

private:
    quint64 m_next = 0;
};

class FakeOutput : public moyu::AudioOutput {
public:
    bool play(const QByteArray& wav) override
    {
        played.push_back(QString::fromUtf8(wav));
        playing = true;
        paused = false;
        return true;
    }
    void setRate(double value) override { rate = value; }
    void pause() override { paused = true; }
    void resume() override { paused = false; }
    void stop() override
    {
        playing = false;
        ++stops;
    }

    void end()
    {
        playing = false;
        Q_EMIT finished();
    }

    std::vector<QString> played;
    bool playing = false;
    bool paused = false;
    double rate = 1.0;
    int stops = 0;
};

}  // namespace

class SpeechTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void segmentsSkipBlankAndSymbolLines();
    void segmentsStartAtAnchorParagraph();
    void segmentsStartAtClickedSentence();
    void longParagraphSplitsAtSentenceEnd();
    void prefetchesOneAheadAndPlaysInOrder();
    void continuesIntoNextChapter();
    void finishesAtBookEnd();
    void rejectedRetriesThenSkips();
    void transientRetriesThenPausesAfterCurrent();
    void authFailureStops();
    void pauseResumeAndStop();
    void bufferingCoversWaitForAudio();
    void statusLineShowsSpeechState();
    void staleResultsAreIgnored();
    void mimoBodyPutsTextInAssistant();
    void mimoReplyClassification();
    void settingsRoundTrip();
    void readerSpeechMarkAndReveal();
    void readerPositionAtClick();
    void macOutputPlaysWavAndFinishes();
    void engineWithoutKeyFailsAsAuth();
    void liveMimoRejectsFakeKey();
    void settingsPreviewSendsTypedText();
};

void SpeechTest::segmentsSkipBlankAndSymbolLines()
{
    const auto content = chapterFrom(0, QStringLiteral("第1章 开始\n\n他走进来。\n* * *\n她笑了。\n"));
    const auto segments = moyu::speechSegments(content, 0);
    QCOMPARE(segments.size(), std::size_t(3));
    QCOMPARE(segments[0].text, QStringLiteral("第1章 开始"));
    QCOMPARE(segments[1].text, QStringLiteral("他走进来。"));
    QCOMPARE(segments[2].text, QStringLiteral("她笑了。"));
    const QByteArray utf8 = content.utf8;
    QCOMPARE(QString::fromUtf8(utf8.mid(segments[2].byteStart, segments[2].byteEnd - segments[2].byteStart)),
             QStringLiteral("她笑了。"));
}

void SpeechTest::segmentsStartAtAnchorParagraph()
{
    const auto content = chapterFrom(0, QStringLiteral("第1章 开始\n第一段。\n第二段很长，锚点在中间。\n第三段。\n"));
    const qsizetype middle = content.utf8.indexOf(QStringLiteral("锚点").toUtf8());
    const auto segments = moyu::speechSegments(content, static_cast<quint32>(middle));
    QCOMPARE(segments.size(), std::size_t(2));
    QCOMPARE(segments[0].text, QStringLiteral("第二段很长，锚点在中间。"));
}

void SpeechTest::segmentsStartAtClickedSentence()
{
    const auto content = chapterFrom(0, QStringLiteral("他站起来。她说：“别走。”外面下雨了。\n下一段。\n"), false);
    const auto from = [&](const QString& needle) {
        return moyu::speechSegments(content, static_cast<quint32>(content.utf8.indexOf(needle.toUtf8())));
    };
    // 点在第二句中间：从第二句句首开始，引号跟着走。
    auto segments = from(QStringLiteral("别走"));
    QCOMPARE(segments.front().text, QStringLiteral("她说：“别走。”外面下雨了。"));
    QCOMPARE(segments.front().byteStart, static_cast<quint32>(content.utf8.indexOf(QStringLiteral("她说").toUtf8())));
    // 点在第三句：从第三句开始。
    segments = from(QStringLiteral("下雨"));
    QCOMPARE(segments.front().text, QStringLiteral("外面下雨了。"));
    QCOMPARE(segments.size(), std::size_t(2));
    // 点在第一句：整段。
    segments = from(QStringLiteral("站起"));
    QCOMPARE(segments.front().text, QStringLiteral("他站起来。她说：“别走。”外面下雨了。"));
}

void SpeechTest::longParagraphSplitsAtSentenceEnd()
{
    QString paragraph;
    for (int i = 0; i < 60; ++i) {
        paragraph += QStringLiteral("他说：“走吧。”");
    }
    const auto content = chapterFrom(0, paragraph + QStringLiteral("\n"), false);
    const auto segments = moyu::speechSegments(content, 0);
    QVERIFY(segments.size() >= 2);
    QString joined;
    for (const auto& segment : segments) {
        QVERIFY(segment.text.size() <= moyu::kMaxSpeechSegmentChars);
        // 句末标点后的引号跟着上一句走。
        QVERIFY2(segment.text.endsWith(QStringLiteral("。”")), qPrintable(segment.text));
        joined += segment.text;
    }
    QCOMPARE(joined, paragraph);
    QCOMPARE(segments.front().byteStart, 0U);
    QCOMPARE(segments.back().byteEnd, static_cast<quint32>(paragraph.toUtf8().size()));
    for (std::size_t i = 1; i < segments.size(); ++i) {
        QCOMPARE(segments[i].byteStart, segments[i - 1].byteEnd);
    }
}

void SpeechTest::prefetchesOneAheadAndPlaysInOrder()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    QSignalSpy started(&speech, &moyu::ReadAloud::segmentStarted);
    speech.start(chapterFrom(0, QStringLiteral("一。\n二。\n三。\n四。\n"), false), 0, 1);

    // 当前段和下一段同时在合成，第三段还没发。
    QCOMPARE(engine.calls.size(), std::size_t(2));
    QCOMPARE(engine.finishOldest(), QStringLiteral("一。"));
    QCOMPARE(output.played, std::vector<QString>{QStringLiteral("一。")});
    QCOMPARE(started.count(), 1);
    QCOMPARE(engine.finishOldest(), QStringLiteral("二。"));
    QCOMPARE(output.played.size(), std::size_t(1));

    output.end();
    QCOMPARE(output.played.back(), QStringLiteral("二。"));
    QCOMPARE(engine.calls.size(), std::size_t(3));
    QCOMPARE(engine.calls.back().text, QStringLiteral("三。"));
}

void SpeechTest::continuesIntoNextChapter()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    QSignalSpy needed(&speech, &moyu::ReadAloud::chapterNeeded);
    speech.start(chapterFrom(0, QStringLiteral("甲。\n乙。\n"), false), 0, 3);
    QCOMPARE(needed.count(), 0);

    engine.finishOldest();
    engine.finishOldest();
    output.end();
    // 只剩一段，要下一章。
    QCOMPARE(needed.count(), 1);
    QCOMPARE(needed.first().first().toUInt(), 1U);
    QVERIFY(speech.waitingForChapter());

    // 不是在等的那章不收。
    speech.offerChapter(chapterFrom(2, QStringLiteral("错。\n"), false));
    QVERIFY(speech.waitingForChapter());
    speech.offerChapter(chapterFrom(1, QStringLiteral("丙。\n丁。\n戊。\n"), false));
    QVERIFY(!speech.waitingForChapter());
    QCOMPARE(engine.calls.back().text, QStringLiteral("丙。"));

    output.end();
    QCOMPARE(output.played.back(), QStringLiteral("乙。"));
    engine.finishOldest();
    QVERIFY(output.playing);
    output.end();
    QCOMPARE(output.played.back(), QStringLiteral("丙。"));
}

void SpeechTest::finishesAtBookEnd()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    QSignalSpy notices(&speech, &moyu::ReadAloud::notice);
    speech.start(chapterFrom(0, QStringLiteral("最后一句。\n"), false), 0, 1);
    engine.finishOldest();
    output.end();
    QCOMPARE(speech.state(), moyu::ReadAloudState::Idle);
    QCOMPARE(notices.last().first().toString(), QStringLiteral("全书读完了"));
}

void SpeechTest::rejectedRetriesThenSkips()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    speech.setRetryScale(0);
    QSignalSpy notices(&speech, &moyu::ReadAloud::notice);
    speech.start(chapterFrom(0, QStringLiteral("脏话。\n正常。\n"), false), 0, 1);

    // 第一次被拦，重试后通过：照常播放。
    engine.finishOldest(moyu::SpeechFailure::Rejected);
    QTRY_COMPARE(engine.calls.size(), std::size_t(3));
    QCOMPARE(engine.calls.back().text, QStringLiteral("脏话。"));
    engine.finishOldest();
    engine.finishOldest();
    QCOMPARE(output.played, std::vector<QString>{QStringLiteral("脏话。")});
    QCOMPARE(notices.count(), 0);

    // 一直被拦：试满次数后跳过，接着读下一段。
    moyu::ReadAloud again(&engine, &output);
    again.setRetryScale(0);
    QSignalSpy againNotices(&again, &moyu::ReadAloud::notice);
    output.played.clear();
    engine.pending.clear();
    again.start(chapterFrom(0, QStringLiteral("脏话。\n正常。\n"), false), 0, 1);
    engine.finishOldest(moyu::SpeechFailure::Rejected);  // 脏话 1
    engine.finishOldest();                                // 正常
    for (int attempt = 2; attempt <= moyu::ReadAloud::kRejectedAttempts; ++attempt) {
        QTRY_VERIFY(!engine.pending.empty());
        engine.finishOldest(moyu::SpeechFailure::Rejected);
    }
    QCOMPARE(output.played, std::vector<QString>{QStringLiteral("正常。")});
    QCOMPARE(againNotices.count(), 1);
}

void SpeechTest::transientRetriesThenPausesAfterCurrent()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    speech.setRetryScale(0);
    QSignalSpy notices(&speech, &moyu::ReadAloud::notice);
    speech.start(chapterFrom(0, QStringLiteral("一。\n二。\n三。\n"), false), 0, 1);
    engine.finishOldest();  // 一 在播
    for (int attempt = 1; attempt <= moyu::ReadAloud::kTransientAttempts; ++attempt) {
        QTRY_VERIFY(!engine.pending.empty());
        QCOMPARE(engine.pending.front().text, QStringLiteral("二。"));
        engine.finishOldest(moyu::SpeechFailure::Transient);
    }
    // 正在播的那段不打断。
    QCOMPARE(speech.state(), moyu::ReadAloudState::Playing);
    QVERIFY(!output.paused);
    output.end();
    QCOMPARE(speech.state(), moyu::ReadAloudState::Paused);
    QCOMPARE(notices.count(), 1);

    // 继续：这一段重新发请求，成功后接着播。
    speech.resume();
    QCOMPARE(engine.pending.front().text, QStringLiteral("二。"));
    engine.finishOldest();
    QCOMPARE(output.played.back(), QStringLiteral("二。"));
}

void SpeechTest::authFailureStops()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    QSignalSpy notices(&speech, &moyu::ReadAloud::notice);
    speech.start(chapterFrom(0, QStringLiteral("一。\n二。\n"), false), 0, 1);
    engine.finishOldest(moyu::SpeechFailure::Auth);
    QCOMPARE(speech.state(), moyu::ReadAloudState::Idle);
    QVERIFY(engine.pending.empty());
    QCOMPARE(notices.count(), 1);
}

void SpeechTest::pauseResumeAndStop()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    speech.start(chapterFrom(0, QStringLiteral("一。\n二。\n"), false), 0, 1);
    speech.pause();
    engine.finishOldest();
    // 暂停时合成回来也不开播。
    QVERIFY(output.played.empty());
    speech.resume();
    QCOMPARE(output.played.size(), std::size_t(1));
    speech.pause();
    QVERIFY(output.paused);
    speech.resume();
    QVERIFY(!output.paused);

    speech.setRate(1.5);
    QCOMPARE(output.rate, 1.5);
    speech.stop();
    QCOMPARE(speech.state(), moyu::ReadAloudState::Idle);
    QVERIFY(!output.playing);
    QCOMPARE(speech.queuedSegments(), 0);
}

void SpeechTest::bufferingCoversWaitForAudio()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    QSignalSpy changed(&speech, &moyu::ReadAloud::bufferingChanged);
    QVERIFY(!speech.buffering());

    // 刚开始：在读但还没声音。
    speech.start(chapterFrom(0, QStringLiteral("一。\n二。\n"), false), 0, 1);
    QVERIFY(speech.buffering());
    QCOMPARE(changed.count(), 1);
    engine.finishOldest();
    QVERIFY(!speech.buffering());

    // 下一段还没合成完，这段先播完了：又在等。
    output.end();
    QVERIFY(speech.buffering());
    // 暂停时不算在等。
    speech.pause();
    QVERIFY(!speech.buffering());
    speech.resume();
    QVERIFY(speech.buffering());
    engine.finishOldest();
    QVERIFY(!speech.buffering());
    speech.stop();
    QVERIFY(!speech.buffering());
}

void SpeechTest::statusLineShowsSpeechState()
{
    const auto content = chapterFrom(0, QStringLiteral("第1章 甲\n正文。\n"));
    moyu::MainWindow window;
    window.showBook({moyu::ChapterIndex{0, QStringLiteral("第1章"), 2, 0, static_cast<quint64>(content.utf8.size()), false}},
                    content,
                    0,
                    static_cast<quint64>(content.utf8.size()),
                    0);
    const auto statusText = [&] {
        QString joined;
        for (QLabel* label : window.findChildren<QLabel*>()) {
            joined += label->text();
        }
        return joined;
    };
    window.setSpeechStatus(QStringLiteral("合成中…"));
    QVERIFY(statusText().contains(QStringLiteral("合成中…")));
    window.setSpeechStatus({});
    QVERIFY(!statusText().contains(QStringLiteral("合成中…")));
}

void SpeechTest::staleResultsAreIgnored()
{
    FakeEngine engine;
    FakeOutput output;
    moyu::ReadAloud speech(&engine, &output);
    speech.start(chapterFrom(0, QStringLiteral("旧。\n"), false), 0, 1);
    const quint64 old = engine.calls.front().id;
    speech.start(chapterFrom(0, QStringLiteral("新。\n"), false), 0, 1);
    moyu::SpeechResult late;
    late.audio = QByteArrayLiteral("old");
    Q_EMIT engine.finished(old, late);
    QVERIFY(output.played.empty());
    engine.finishOldest();
    QCOMPARE(output.played, std::vector<QString>{QStringLiteral("新。")});
}

void SpeechTest::mimoBodyPutsTextInAssistant()
{
    moyu::MimoConfig config;
    config.voice = QStringLiteral("茉莉");
    config.style = QStringLiteral("温柔");
    const QJsonObject body = QJsonDocument::fromJson(moyu::mimoRequestBody(config, QStringLiteral("正文"))).object();
    QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("mimo-v2.5-tts"));
    const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    QCOMPARE(messages.at(0).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("温柔"));
    QCOMPARE(messages.at(1).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
    QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toString(), QStringLiteral("正文"));
    const QJsonObject audio = body.value(QStringLiteral("audio")).toObject();
    QCOMPARE(audio.value(QStringLiteral("format")).toString(), QStringLiteral("wav"));
    QCOMPARE(audio.value(QStringLiteral("voice")).toString(), QStringLiteral("茉莉"));

    config.style.clear();
    const QJsonObject bare = QJsonDocument::fromJson(moyu::mimoRequestBody(config, QStringLiteral("正文"))).object();
    QCOMPARE(bare.value(QStringLiteral("messages")).toArray().size(), 1);
}

void SpeechTest::mimoReplyClassification()
{
    using moyu::SpeechFailure;
    const QByteArray ok = QByteArrayLiteral(R"({"choices":[{"finish_reason":"stop","message":{"audio":{"data":"UklGRg=="}}}]})");
    const auto good = moyu::parseMimoReply(200, ok, {});
    QCOMPARE(good.failure, SpeechFailure::None);
    QCOMPARE(good.audio, QByteArrayLiteral("RIFF"));

    // 实际碰到过的审核拦截返回。
    const QByteArray filtered = QByteArrayLiteral(
        R"({"id":"","choices":[{"finish_reason":"content_filter","index":0,"message":{"content":"The request was rejected because it was considered high risk","role":"assistant"}}]})");
    QCOMPARE(moyu::parseMimoReply(200, filtered, {}).failure, SpeechFailure::Rejected);

    const QByteArray invalidKey = QByteArrayLiteral(R"({"error":{"message":"Invalid API Key","code":"401"}})");
    QCOMPARE(moyu::parseMimoReply(401, invalidKey, {}).failure, SpeechFailure::Auth);
    QCOMPARE(moyu::parseMimoReply(0, {}, QStringLiteral("timeout")).failure, SpeechFailure::Transient);
    QCOMPARE(moyu::parseMimoReply(429, {}, {}).failure, SpeechFailure::Transient);
    QCOMPARE(moyu::parseMimoReply(503, {}, {}).failure, SpeechFailure::Transient);
    const auto bad = moyu::parseMimoReply(400, QByteArrayLiteral(R"({"error":{"message":"Invalid JSON in request body"}})"), {});
    QCOMPARE(bad.failure, SpeechFailure::Fatal);
    QVERIFY(bad.message.contains(QStringLiteral("Invalid JSON")));
    QCOMPARE(moyu::parseMimoReply(200, QByteArrayLiteral(R"({"choices":[]})"), {}).failure, SpeechFailure::Fatal);
}

void SpeechTest::settingsRoundTrip()
{
    moyu::SpeechSettings settings;
    settings.apiKey = QStringLiteral("sk-test");
    settings.voice = QStringLiteral("苏打");
    settings.style = QStringLiteral("评书");
    settings.ratePercent = 130;
    settings.mark = true;
    settings.markColor = QColor(10, 20, 30, 40);
    const auto restored = moyu::speechSettingsFromItems(moyu::speechSettingsItems(settings), {});
    QCOMPARE(restored.apiKey, settings.apiKey);
    QCOMPARE(restored.voice, settings.voice);
    QCOMPARE(restored.style, settings.style);
    QCOMPARE(restored.ratePercent, 130);
    QVERIFY(restored.mark);
    QCOMPARE(restored.markColor, settings.markColor);

    // 默认：茉莉、跟读标记关。不认识的音色和越界语速不收。
    const auto fallback = moyu::speechSettingsFromItems(
        {{QStringLiteral("speech/voice"), QStringLiteral("不存在")}, {QStringLiteral("speech/rate"), QStringLiteral("999")}}, {});
    QCOMPARE(fallback.voice, QStringLiteral("茉莉"));
    QCOMPARE(fallback.ratePercent, 200);
    QVERIFY(!fallback.mark);
    QCOMPARE(moyu::SpeechSettings{}.effectiveStyle(), QString::fromUtf8(moyu::kDefaultSpeechStyle));
}

void SpeechTest::readerSpeechMarkAndReveal()
{
    QString text = QStringLiteral("第1章 开始\n");
    for (int i = 0; i < 200; ++i) {
        text += QStringLiteral("第%1段，正文。\n").arg(i);
    }
    const auto content = chapterFrom(0, text);
    moyu::ReaderView view;
    view.resize(320, 240);
    view.setDocument(content, 0, static_cast<quint64>(content.utf8.size()));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QVERIFY(view.loadedChapter(0) != nullptr);
    QVERIFY(view.loadedChapter(1) == nullptr);
    QVERIFY(!view.revealByte(1, 0));

    // 已经在屏幕上的不动。
    const quint32 near = static_cast<quint32>(content.utf8.indexOf(QStringLiteral("第1段").toUtf8()));
    QVERIFY(view.revealByte(0, near));
    QCOMPARE(view.byteOffsetInChapter(), 0U);

    // 不在屏幕上的翻过去。
    const quint32 far = static_cast<quint32>(content.utf8.indexOf(QStringLiteral("第150段").toUtf8()));
    QVERIFY(view.revealByte(0, far));
    QCOMPARE(view.byteOffsetInChapter(), far);

    view.setSpeechMark(0, far, 10, QColor(1, 2, 3, 4));
    QVERIFY(view.hasSpeechMark());
    QVERIFY(!view.hasHighlight());
    view.clearSpeechMark();
    QVERIFY(!view.hasSpeechMark());
}

namespace {

// 24kHz 单声道 16 位的静音 WAV，和 MiMo 返回的格式一样。
QByteArray silentWav(int milliseconds)
{
    const quint32 rate = 24000;
    const quint32 samples = rate * static_cast<quint32>(milliseconds) / 1000;
    const quint32 dataBytes = samples * 2;
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate * 2) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << dataBytes;
    wav.append(QByteArray(static_cast<qsizetype>(dataBytes), '\0'));
    return wav;
}

}  // namespace

void SpeechTest::macOutputPlaysWavAndFinishes()
{
    moyu::MacAudioOutput output;
    QSignalSpy finished(&output, &moyu::AudioOutput::finished);
    QVERIFY(!output.play(QByteArrayLiteral("not a wav")));
    output.setRate(2.0);
    QVERIFY(output.play(silentWav(300)));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);

    // stop() 不发 finished。
    QVERIFY(output.play(silentWav(300)));
    output.stop();
    QTest::qWait(500);
    QCOMPARE(finished.count(), 1);
}

void SpeechTest::engineWithoutKeyFailsAsAuth()
{
    moyu::MimoSpeechEngine engine;
    QSignalSpy finished(&engine, &moyu::SpeechEngine::finished);
    const quint64 id = engine.synthesize(QStringLiteral("你好"));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toULongLong(), id);
    QCOMPARE(finished.first().at(1).value<moyu::SpeechResult>().failure, moyu::SpeechFailure::Auth);
}

// 默认跳过。设置 MOYU_LIVE_MIMO=1 时真的连一次 MiMo，用假 Key 确认 TLS、直连和 401 分类都对。
void SpeechTest::liveMimoRejectsFakeKey()
{
    if (qEnvironmentVariableIsEmpty("MOYU_LIVE_MIMO")) {
        QSKIP("设置 MOYU_LIVE_MIMO=1 才连真实服务");
    }
    moyu::MimoSpeechEngine engine;
    moyu::MimoConfig config;
    config.apiKey = QStringLiteral("invalid-test-key");
    engine.setConfig(config);
    QSignalSpy finished(&engine, &moyu::SpeechEngine::finished);
    engine.synthesize(QStringLiteral("你好"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
    const auto result = finished.first().at(1).value<moyu::SpeechResult>();
    QVERIFY2(result.failure == moyu::SpeechFailure::Auth, qPrintable(result.message));
}

void SpeechTest::settingsPreviewSendsTypedText()
{
    moyu::SettingsDialog dialog;
    auto* text = dialog.findChild<QPlainTextEdit*>();
    QPushButton* button = nullptr;
    for (QPushButton* candidate : dialog.findChildren<QPushButton*>()) {
        if (candidate->text() == QStringLiteral("试听")) {
            button = candidate;
        }
    }
    QVERIFY(text != nullptr);
    QVERIFY(button != nullptr);
    QVERIFY(!text->toPlainText().isEmpty());
    QSignalSpy requested(&dialog, &moyu::SettingsDialog::speechPreviewRequested);
    QSignalSpy stopped(&dialog, &moyu::SettingsDialog::speechPreviewStopRequested);

    text->setPlainText(QStringLiteral("  我自己写的一句话。  "));
    button->click();
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.first().first().toString(), QStringLiteral("我自己写的一句话。"));

    // 合成或播放中，按钮变成停止。
    dialog.setPreviewState(moyu::PreviewState::Synthesizing);
    QCOMPARE(button->text(), QStringLiteral("停止"));
    button->click();
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(requested.count(), 1);
    dialog.setPreviewState(moyu::PreviewState::Idle, QStringLiteral("试听失败：测试"));
    QCOMPARE(button->text(), QStringLiteral("试听"));
    bool shown = false;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        shown = shown || label->text() == QStringLiteral("试听失败：测试");
    }
    QVERIFY(shown);

    // 空的、太长的都不发。
    text->setPlainText(QStringLiteral("   "));
    button->click();
    text->setPlainText(QString(moyu::kMaxSpeechSegmentChars + 1, QChar(u'字')));
    button->click();
    QCOMPARE(requested.count(), 1);
}

void SpeechTest::readerPositionAtClick()
{
    QString text = QStringLiteral("第1章 开始\n");
    for (int i = 0; i < 60; ++i) {
        text += QStringLiteral("第%1段，正文。\n").arg(i, 2, 10, QChar(u'0'));
    }
    const auto content = chapterFrom(0, text);
    for (const bool page : {false, true}) {
        moyu::ReaderView view;
        view.resize(320, 240);
        view.setDocument(content, 0, static_cast<quint64>(content.utf8.size()));
        view.setReadMode(page ? moyu::ReadMode::Page : moyu::ReadMode::Scroll);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const quint32 target = static_cast<quint32>(content.utf8.indexOf(QStringLiteral("第30段").toUtf8()));
        view.scrollToByteOffset(target);
        QApplication::processEvents();

        // 顶上一行的最左边就是这段的开头。
        const auto top = view.positionAt(QPoint(1, 2));
        QVERIFY(top.has_value());
        QCOMPARE(top->chapter, 0U);
        QCOMPARE(top->byte, target);
        // 往下一点是后面的段。
        const auto lower = view.positionAt(QPoint(1, 120));
        QVERIFY(lower.has_value());
        QVERIFY2(lower->byte > target, page ? "page" : "scroll");
        // 同一行往右是同一段里靠后的字。
        const auto right = view.positionAt(QPoint(80, 2));
        QVERIFY(right.has_value());
        QVERIFY(right->byte > target);
        QVERIFY(right->byte < static_cast<quint32>(content.utf8.indexOf(QStringLiteral("第31段").toUtf8())));
    }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    SpeechTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_speech.moc"
