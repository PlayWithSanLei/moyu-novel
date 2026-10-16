#include "app/Application.h"
#include "app/StartupTiming.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    moyu::markStartup();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MoyuReader"));
    QApplication::setOrganizationName(QStringLiteral("MoyuReader"));
    QApplication::setApplicationDisplayName(QStringLiteral("摸鱼阅读器"));

    moyu::Application application;
    return application.run();
}
