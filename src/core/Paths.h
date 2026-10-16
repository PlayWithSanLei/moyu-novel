#pragma once

#include <QDir>
#include <QString>

namespace moyu {

// 不走 QStandardPaths。组织名或应用名一改，缓存和数据库就会换地方。
inline QString databaseFilePath()
{
    return QDir::homePath() + QStringLiteral("/Library/Application Support/MoyuReader/moyu.db");
}

inline QString cacheDirectoryPath()
{
    return QDir::homePath() + QStringLiteral("/Library/Caches/MoyuReader");
}

// token 里只有数字、下划线和十六进制。冒号不能进文件名。
inline QString utf8CachePath(const QString& cacheDir, const QString& token)
{
    return cacheDir + QLatin1Char('/') + token + QStringLiteral(".utf8");
}

}  // namespace moyu
