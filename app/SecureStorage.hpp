#pragma once

#include <QByteArray>
#include <QString>

namespace account::securestorage {

/// Stores the complete desktop credential envelope in the operating system's
/// credential vault. Secret material must never fall back to QSettings.
bool write(const QByteArray& value);
QByteArray read();
bool clear();

/// Named secret slots share the same operating-system vault but never the
/// account-session record. AI connections use one slot per local model so a
/// model can be replaced or removed without exposing or rewriting the login.
bool writeNamed(const QString& name, const QByteArray& value);
QByteArray readNamed(const QString& name);
bool clearNamed(const QString& name);

} // namespace account::securestorage
