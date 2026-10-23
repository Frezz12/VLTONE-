#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QString>

namespace vlt::mcp {
inline QString endpointName() {
    return QStringLiteral("vltone-mcp-") + QString::fromLatin1(
        QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
}
}
