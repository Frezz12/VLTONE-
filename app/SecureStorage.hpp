#pragma once

#include <QByteArray>
#include <QString>

namespace account::securestorage {

// Automatic restore/refresh must never open an operating-system password
// dialog. Only explicit account or API-key changes may allow one.
enum class Interaction { Disallow, Allow };
struct ReadResult {
    QByteArray value;
    bool unavailable = false;
};
/// Distinguish a missing session from a locked/inaccessible vault.
ReadResult readSession(Interaction interaction = Interaction::Disallow);

/// Stores the complete desktop credential envelope in the operating system's
/// credential vault. Secret material must never fall back to QSettings.
bool write(const QByteArray& value, Interaction interaction = Interaction::Disallow);
QByteArray read();
bool clear(Interaction interaction = Interaction::Disallow);

/// Named secret slots share the same operating-system vault but never the
/// account-session record. AI connections use one slot per local model so a
/// model can be replaced or removed without exposing or rewriting the login.
bool writeNamed(const QString& name, const QByteArray& value,
                Interaction interaction = Interaction::Disallow);
QByteArray readNamed(const QString& name, Interaction interaction = Interaction::Disallow);
bool clearNamed(const QString& name, Interaction interaction = Interaction::Disallow);

} // namespace account::securestorage
