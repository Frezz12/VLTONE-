#pragma once

#include <QByteArray>
#include <QString>

namespace account::securestorage {

// Automatic restore/refresh must never open an operating-system password
// dialog. macOS account sessions always use a local file and ignore this flag.
enum class Interaction { Disallow, Allow };
struct ReadResult {
    QByteArray value;
    bool unavailable = false;
};
/// Distinguish a missing session from inaccessible credential storage.
ReadResult readSession(Interaction interaction = Interaction::Disallow);

/// Stores the complete desktop credential envelope in a private local file on
/// macOS (unencrypted, directory 0700/file 0600), or the Windows credential vault.
/// macOS never reads or migrates the legacy Keychain account record.
bool write(const QByteArray& value, Interaction interaction = Interaction::Disallow);
QByteArray read();
bool clear(Interaction interaction = Interaction::Disallow);

/// Named API-key slots use the operating-system vault. AI connections use one
/// slot per local model so a model can be replaced or removed without exposing
/// or rewriting the login.
bool writeNamed(const QString& name, const QByteArray& value,
                Interaction interaction = Interaction::Disallow);
QByteArray readNamed(const QString& name, Interaction interaction = Interaction::Disallow);
bool clearNamed(const QString& name, Interaction interaction = Interaction::Disallow);

} // namespace account::securestorage
