// Compile the real vault implementation against a fake Security API. No test
// operation can read, modify, unlock, or display a dialog for a user's keychain.
#include <Security/Security.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
QString localDataDirectory;
struct TestStandardPaths {
    static constexpr auto AppLocalDataLocation = QStandardPaths::AppLocalDataLocation;
    static QString writableLocation(QStandardPaths::StandardLocation) { return localDataDirectory; }
};
Boolean interactionAllowed = true;
bool expectInteractive = false;
OSStatus getPolicyStatus = errSecSuccess;
OSStatus setPolicyStatus = errSecSuccess;
OSStatus readStatus = errSecSuccess;
OSStatus updateStatus = errSecSuccess;
OSStatus addStatus = errSecSuccess;
OSStatus deleteStatus = errSecSuccess;
int vaultCalls = 0;
int adds = 0;
int policyChanges = 0;
OSStatus accessStatus = errSecSuccess;
bool expectAccess = false;

OSStatus fakeCreateAccess(CFStringRef, CFArrayRef trusted, SecAccessRef* result) {
    check(trusted == nullptr, "trust only the current application, never every app");
    if (accessStatus == errSecSuccess)
        *result = reinterpret_cast<SecAccessRef>(CFStringCreateMutableCopy(nullptr, 0, CFSTR("test-access")));
    return accessStatus;
}

void checkAccess(CFDictionaryRef attributes) {
    check(CFDictionaryContainsKey(attributes, kSecAttrAccess) == expectAccess,
          "explicit save repairs the ACL; background refresh preserves it");
}

OSStatus fakeGetPolicy(Boolean* value) {
    *value = interactionAllowed;
    return getPolicyStatus;
}
OSStatus fakeSetPolicy(Boolean value) {
    ++policyChanges;
    if (setPolicyStatus == errSecSuccess) interactionAllowed = value;
    return setPolicyStatus;
}
void vaultCall(CFDictionaryRef query) {
    ++vaultCalls;
    check(bool(interactionAllowed) == expectInteractive, "incorrect interaction policy during vault call");
    check(CFEqual(CFDictionaryGetValue(query, kSecAttrService),
                  CFSTR("com.vltstudio.pro.desktop-account")), "service must stay compatible");
}
OSStatus fakeRead(CFDictionaryRef query, CFTypeRef* result) {
    vaultCall(query);
    if (readStatus == errSecSuccess) {
        constexpr UInt8 bytes[] = {'t', 'e', 's', 't'};
        *result = CFDataCreate(kCFAllocatorDefault, bytes, sizeof(bytes));
    }
    return readStatus;
}
OSStatus fakeUpdate(CFDictionaryRef query, CFDictionaryRef attributes) {
    vaultCall(query);
    checkAccess(attributes);
    return updateStatus;
}
OSStatus fakeAdd(CFDictionaryRef query, CFTypeRef*) {
    vaultCall(query);
    checkAccess(query);
    ++adds;
    return addStatus;
}
OSStatus fakeDelete(CFDictionaryRef query) {
    vaultCall(query);
    return deleteStatus;
}
}

#define SecKeychainGetUserInteractionAllowed fakeGetPolicy
#define SecKeychainSetUserInteractionAllowed fakeSetPolicy
#define SecItemCopyMatching fakeRead
#define SecItemUpdate fakeUpdate
#define SecItemAdd fakeAdd
#define SecItemDelete fakeDelete
#define SecAccessCreate fakeCreateAccess
#define QStandardPaths TestStandardPaths
#include "../app/SecureStorage.cpp"
#undef QStandardPaths
#undef SecKeychainGetUserInteractionAllowed
#undef SecKeychainSetUserInteractionAllowed
#undef SecItemCopyMatching
#undef SecItemUpdate
#undef SecItemAdd
#undef SecItemDelete
#undef SecAccessCreate

// Exercise the unchanged AI-key vault separately from the local account slot.
namespace {
QByteArray vaultRead() { return account::securestorage::readNamed("ai-test"); }
account::securestorage::ReadResult vaultReadSession(account::securestorage::Interaction mode = account::securestorage::Interaction::Disallow) {
    auto value = account::securestorage::readNamed("ai-test", mode);
    return {value, account::securestorage::readUnavailable};
}
bool vaultWrite(const QByteArray& value, account::securestorage::Interaction mode = account::securestorage::Interaction::Disallow) {
    return account::securestorage::writeNamed("ai-test", value, mode);
}
bool vaultClear(account::securestorage::Interaction mode = account::securestorage::Interaction::Disallow) {
    return account::securestorage::clearNamed("ai-test", mode);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace account::securestorage;
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--read-local") {
        localDataDirectory = app.arguments().at(2);
        check(read() == QByteArray("rotated-session"), "fresh process restores the saved session");
        check(vaultCalls == 0 && policyChanges == 0, "restart never accesses Keychain");
        return 0;
    }
    QTemporaryDir storage;
    check(storage.isValid(), "isolated local credential directory");
    localDataDirectory = storage.path();
    const QString directory = storage.filePath("credentials");
    const QString path = directory + "/desktop-session.json";
    check(read().isEmpty() && !readSession().unavailable, "missing local session requires first sign-in");
    check(clear(), "logout without a local session succeeds");
    check(write("first-session", Interaction::Allow), "explicit login saves locally");
    check(write("rotated-session"), "refresh replaces local session");
    check(readSession(Interaction::Allow).value == "rotated-session", "explicit restore reads locally");
    struct stat info{};
    check(::stat(QFile::encodeName(path).constData(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "session is readable and writable only by its owner");
    check(::stat(QFile::encodeName(directory).constData(), &info) == 0 && (info.st_mode & 0777) == 0700,
          "credential directory is private");
    QProcess restarted;
    restarted.start(QCoreApplication::applicationFilePath(), {"--read-local", storage.path()});
    check(restarted.waitForFinished(10000) && restarted.exitStatus() == QProcess::NormalExit && restarted.exitCode() == 0,
          "credentials survive a process restart");
    check(!write({}) && !write(QByteArray(1024 * 1024 + 1, 'x')) && read() == "rotated-session",
          "invalid replacement leaves previous credential intact");
    check(QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() == 1,
          "successful writes leave no temporary credential files");
    check(clear(Interaction::Allow) && !QFile::exists(path) && read().isEmpty() && !readSession().unavailable,
          "logout removes the file and never resurrects the old Keychain session");
    check(QDir().mkdir(path), "simulate failed atomic replacement");
    check(!write("failed") && readSession().unavailable && !clear(), "storage errors are reported");
    check(QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
          "failed writes remove temporary credentials");
    check(QDir().rmdir(path), "remove blocked destination");
    QFile outside(storage.filePath("outside"));
    check(outside.open(QIODevice::WriteOnly) && outside.write("unrelated") == 9, "create symlink target");
    outside.close();
    check(QFile::link(outside.fileName(), path), "simulate symlink session");
    check(readSession().unavailable, "session reads do not follow symlinks");
    check(write("replacement") && read() == "replacement", "atomic writes replace symlinks without following them");
    check(outside.open(QIODevice::ReadOnly) && outside.readAll() == "unrelated", "symlink target remains untouched");
    outside.close();
    check(clear() && QDir().rmdir(directory), "remove private session directory");
    check(QFile::link(storage.path(), directory), "simulate symlink credential directory");
    check(!write("denied") && readSession().unavailable && !clear(), "symlink directories are rejected");
    check(QFile::remove(directory), "remove directory symlink");
    check(vaultCalls == 0 && policyChanges == 0, "all account operations bypass Keychain even when interaction is allowed");

    check(vaultRead() == QByteArray("test"), "authorized startup restores credentials");
    check(interactionAllowed, "read restores the previous policy");

    readStatus = errSecInteractionNotAllowed;
    check(vaultReadSession().unavailable, "locked vault is distinguished from missing sign-in without UI");
    check(interactionAllowed, "denied read restores the previous policy");
    readStatus = errSecItemNotFound;
    check(readNamed(QStringLiteral("ai-test")).isEmpty(), "missing API key returns empty");
    check(!vaultReadSession().unavailable, "missing session is not a vault access failure");

    check(vaultWrite(QByteArray("test")), "background token refresh writes silently");
    updateStatus = errSecItemNotFound;
    check(vaultWrite(QByteArray("test")), "new credential is added silently");
    check(adds == 1 && interactionAllowed, "add restores policy");
    updateStatus = errSecInteractionNotAllowed;
    check(!vaultWrite(QByteArray("test")) && adds == 1,
          "inaccessible credential must not be replaced or downgraded");

    updateStatus = errSecSuccess;
    expectInteractive = true;
    readStatus = errSecSuccess;
    check(vaultReadSession(Interaction::Allow).value == QByteArray("test"), "explicit restore may unlock the existing credential");
    expectAccess = true;
    check(vaultWrite(QByteArray("test"), Interaction::Allow), "explicit sign-in may request authorization");
    updateStatus = errSecItemNotFound;
    check(vaultWrite(QByteArray("test"), Interaction::Allow), "new login trusts the current app");
    updateStatus = errSecSuccess;
    accessStatus = errSecNotAvailable;
    const int beforeAccessFailure = vaultCalls;
    check(!vaultWrite(QByteArray("test"), Interaction::Allow), "ACL creation failure is reported");
    check(vaultCalls == beforeAccessFailure, "failed ACL creation must not modify credentials");
    accessStatus = errSecSuccess;
    check(interactionAllowed, "explicit sign-in preserves policy");
    check(vaultClear(Interaction::Allow), "explicit logout may authorize deletion");
    expectInteractive = false;
    check(vaultClear(), "cleanup does not open dialogs");
    deleteStatus = errSecInteractionNotAllowed;
    check(!vaultClear(), "denied deletion is reported");
    check(interactionAllowed, "failed deletion restores policy");

    interactionAllowed = false;
    const int changes = policyChanges;
    readStatus = errSecSuccess;
    check(vaultRead() == QByteArray("test"), "read works with an existing noninteractive policy");
    check(vaultWrite(QByteArray("test"), Interaction::Allow), "explicit write respects an existing noninteractive policy");
    check(!interactionAllowed && policyChanges == changes, "do not enable UI disabled by another component");
    expectAccess = false;

    interactionAllowed = true;
    int calls = vaultCalls;
    getPolicyStatus = errSecNotAvailable;
    check(vaultRead().isEmpty(), "policy lookup failure fails closed");
    check(!vaultWrite(QByteArray("test")) && !vaultClear(), "mutations fail closed too");
    check(vaultCalls == calls && interactionAllowed, "no vault access if policy cannot be read");
    getPolicyStatus = errSecSuccess;
    setPolicyStatus = errSecNotAvailable;
    check(vaultRead().isEmpty() && !vaultWrite(QByteArray("test")) && !vaultClear(), "policy change failure fails closed");
    check(vaultCalls == calls && interactionAllowed, "no vault access if UI cannot be disabled");
    std::puts("secure_storage_mac_test: PASS");
}
