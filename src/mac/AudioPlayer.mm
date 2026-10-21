#include "mac/AudioPlayer.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <functional>

#undef slots
#undef signals
#undef emit
#import <AVFoundation/AVFoundation.h>

// 没开 ARC，retain/release 手动配对。
@interface MoyuAudioDelegate : NSObject <AVAudioPlayerDelegate> {
@public
    std::function<void(AVAudioPlayer*)> onEnd;
}
@end

@implementation MoyuAudioDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)flag
{
    (void)flag;
    if (onEnd) {
        onEnd(player);
    }
}

- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)player error:(NSError*)error
{
    (void)error;
    if (onEnd) {
        onEnd(player);
    }
}
@end

namespace moyu {

struct MacAudioOutput::Impl {
    AVAudioPlayer* player = nil;
    MoyuAudioDelegate* delegate = nil;

    void release()
    {
        if (player != nil) {
            player.delegate = nil;
            [player stop];
            [player release];
            player = nil;
        }
    }
};

MacAudioOutput::MacAudioOutput(QObject* parent)
    : AudioOutput(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_impl->delegate = [[MoyuAudioDelegate alloc] init];
    QPointer<MacAudioOutput> self(this);
    m_impl->delegate->onEnd = [self](AVAudioPlayer* player) {
        // 代理回调不保证在 Qt 线程上，排回对象所在线程再处理。
        QMetaObject::invokeMethod(
            self.data(),
            [self, player] {
                if (self == nullptr || self->m_impl->player != player) {
                    return;
                }
                self->m_impl->release();
                Q_EMIT self->finished();
            },
            Qt::QueuedConnection);
    };
}

MacAudioOutput::~MacAudioOutput()
{
    m_impl->release();
    m_impl->delegate->onEnd = nullptr;
    [m_impl->delegate release];
    m_impl->delegate = nil;
}

bool MacAudioOutput::play(const QByteArray& wav)
{
    m_impl->release();
    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:wav.constData() length:static_cast<NSUInteger>(wav.size())];
        NSError* error = nil;
        AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:data error:&error];
        if (player == nil) {
            return false;
        }
        player.delegate = m_impl->delegate;
        player.enableRate = YES;
        player.rate = static_cast<float>(m_rate);
        m_impl->player = player;
        if (![player prepareToPlay] || ![player play]) {
            m_impl->release();
            return false;
        }
    }
    return true;
}

void MacAudioOutput::setRate(double rate)
{
    m_rate = std::clamp(rate, 0.5, 2.0);
    if (m_impl->player != nil) {
        m_impl->player.rate = static_cast<float>(m_rate);
    }
}

void MacAudioOutput::pause()
{
    if (m_impl->player != nil) {
        [m_impl->player pause];
    }
}

void MacAudioOutput::resume()
{
    if (m_impl->player != nil) {
        [m_impl->player play];
    }
}

void MacAudioOutput::stop()
{
    m_impl->release();
}

}  // namespace moyu
