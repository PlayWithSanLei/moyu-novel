#include "app/StartupTiming.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QTimer>

#include <chrono>
#include <cstdio>

namespace moyu {
namespace {

std::chrono::steady_clock::time_point g_start;
bool g_marked = false;
bool g_logged = false;

bool envIsOne(const char* name)
{
    return qgetenv(name) == "1";
}

}  // namespace

void markStartup()
{
    g_start = std::chrono::steady_clock::now();
    g_marked = true;
}

void noteTextPainted()
{
    if (g_logged || !g_marked) {
        return;
    }
    g_logged = true;
    if (envIsOne("MOYU_TIMING")) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
        std::fprintf(stderr, "moyu_startup_paint_ms %.3f\n", ms);
        std::fflush(stderr);
    }
    // 索引还没写完就退出，下一次启动会把整本再转码一遍。
    if (envIsOne("MOYU_QUIT_AFTER_PAINT") && !envIsOne("MOYU_QUIT_AFTER_INDEX") &&
        QCoreApplication::instance() != nullptr) {
        QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::quit(); });
    }
}

void noteIndexReady()
{
    if (!envIsOne("MOYU_QUIT_AFTER_INDEX") || QCoreApplication::instance() == nullptr) {
        return;
    }
    QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::quit(); });
}

}  // namespace moyu
