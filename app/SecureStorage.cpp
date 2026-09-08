#include "SecureStorage.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace account::securestorage {

namespace {
const QString kAccountSlot = QStringLiteral("desktop-session");
thread_local bool readUnavailable = false;
}

#if defined(Q_OS_WIN)
namespace {
std::wstring credentialTarget(const QString& name) {
    if (name == kAccountSlot) return std::wstring(L"VLTStudioPro/DesktopAccount");
    return (QStringLiteral("VLTStudioPro/") + name).toStdWString();
}
}

bool writeNamed(const QString& name, const QByteArray& value, Interaction) {
    if (value.isEmpty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    const std::wstring target = credentialTarget(name);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = DWORD(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"VLTONE");
    return CredWriteW(&credential, 0) != FALSE;
}

QByteArray readNamed(const QString& name, Interaction) {
    readUnavailable = false;
    const std::wstring target = credentialTarget(name);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        readUnavailable = GetLastError() != ERROR_NOT_FOUND;
        return {};
    }
    const QByteArray value(reinterpret_cast<const char*>(credential->CredentialBlob),
                           int(credential->CredentialBlobSize));
    CredFree(credential);
    return value;
}

bool clearNamed(const QString& name, Interaction) {
    const std::wstring target = credentialTarget(name);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE ||
           GetLastError() == ERROR_NOT_FOUND;
}

#elif defined(Q_OS_MACOS)
namespace {
// The account session intentionally uses a local, unencrypted file on macOS.
// Never consult the old Keychain slot, including after logout or a missing file.
// Named AI keys below retain their existing vault backend.
class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value(value) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    const int value;
};

int sessionDirectory(bool create) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) { errno = EACCES; return -1; }
    if (create && !QDir().mkpath(root)) { errno = EACCES; return -1; }
    const auto path = QFile::encodeName(QDir(root).filePath(QStringLiteral("credentials")));
    if (create && ::mkdir(path.constData(), 0700) != 0 && errno != EEXIST) return -1;
    FileDescriptor directory(::open(path.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.value < 0) return -1;
    struct stat info{};
    if (::fstat(directory.value, &info) != 0 || info.st_uid != ::geteuid() ||
        ::fchmod(directory.value, 0700) != 0) {
        errno = EACCES;
        return -1;
    }
    return ::fcntl(directory.value, F_DUPFD_CLOEXEC, 0);
}

constexpr auto kSessionFile = "desktop-session.json";
constexpr qint64 kMaxSessionBytes = 1024 * 1024;

QByteArray readLocalSession() {
    readUnavailable = false;
    FileDescriptor directory(sessionDirectory(false));
    if (directory.value < 0) { readUnavailable = errno != ENOENT; return {}; }
    FileDescriptor descriptor(::openat(directory.value, kSessionFile,
        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (descriptor.value < 0) { readUnavailable = errno != ENOENT; return {}; }
    struct stat info{};
    if (::fstat(descriptor.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != ::geteuid() || info.st_nlink != 1 ||
        info.st_size <= 0 || info.st_size > kMaxSessionBytes ||
        ::fchmod(descriptor.value, 0600) != 0) {
        readUnavailable = true;
        return {};
    }
    QFile file;
    if (!file.open(descriptor.value, QIODevice::ReadOnly)) { readUnavailable = true; return {}; }
    const auto value = file.read(kMaxSessionBytes + 1);
    readUnavailable = file.error() != QFileDevice::NoError || value.size() != info.st_size;
    return readUnavailable ? QByteArray{} : value;
}

bool writeLocalSession(const QByteArray& value) {
    if (value.isEmpty() || value.size() > kMaxSessionBytes) return false;
    FileDescriptor directory(sessionDirectory(true));
    if (directory.value < 0) return false;
    const auto temporary = QByteArray(".session-") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    FileDescriptor descriptor(::openat(directory.value, temporary.constData(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (descriptor.value < 0) return false;
    QFile file;
    // Sync the complete replacement before publishing it. A failed write must
    // leave the previous refresh token (and retry request ID) intact.
    const bool saved = ::fchmod(descriptor.value, 0600) == 0 &&
        file.open(descriptor.value, QIODevice::WriteOnly) &&
        file.write(value) == value.size() && file.flush() &&
        ::fsync(descriptor.value) == 0 &&
        ::renameat(directory.value, temporary.constData(), directory.value, kSessionFile) == 0;
    if (!saved) ::unlinkat(directory.value, temporary.constData(), 0);
    return saved && ::fsync(directory.value) == 0;
}

bool clearLocalSession() {
    FileDescriptor directory(sessionDirectory(false));
    if (directory.value < 0) return errno == ENOENT;
    if (::unlinkat(directory.value, kSessionFile, 0) != 0) return errno == ENOENT;
    return ::fsync(directory.value) == 0;
}

// These records live in the legacy login keychain. The per-query Data
// Protection authentication flags do not suppress its access dialogs.
// Scope the legacy process-wide flag to each synchronous operation and
// serialize our vault calls so that they cannot restore each other's flag.
// Other users of Security.framework retain their previous policy afterwards.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
class KeychainInteractionScope {
public:
    explicit KeychainInteractionScope(Interaction interaction)
        : m_lock(m_mutex) {
        if (interaction == Interaction::Allow) {
            m_ready = true;
        } else if (SecKeychainGetUserInteractionAllowed(&m_previous) == errSecSuccess) {
            m_ready = !m_previous ||
                SecKeychainSetUserInteractionAllowed(false) == errSecSuccess;
            m_restore = m_ready && m_previous;
        }
    }
    ~KeychainInteractionScope() {
        if (m_restore) SecKeychainSetUserInteractionAllowed(m_previous);
    }
    bool ready() const { return m_ready; }

private:
    inline static std::mutex m_mutex;
    std::unique_lock<std::mutex> m_lock;
    Boolean m_previous = false;
    bool m_ready = false;
    bool m_restore = false;
};
#pragma clang diagnostic pop

CFStringRef service() { return CFSTR("com.vltstudio.pro.desktop-account"); }

CFMutableDictionaryRef baseQuery(const QString& name) {
    auto query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const QByteArray utf8 = name.toUtf8();
    CFStringRef account = CFStringCreateWithCString(
        kCFAllocatorDefault, utf8.constData(), kCFStringEncodingUTF8);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service());
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(account);
    return query;
}
}

bool writeNamed(const QString& name, const QByteArray& value, Interaction interaction) {
    if (name == kAccountSlot) return writeLocalSession(value);
    if (value.isEmpty()) return false;
    KeychainInteractionScope scope(interaction);
    if (!scope.ready()) return false;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.constData()), value.size());
    CFMutableDictionaryRef query = baseQuery(name);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    // Updating the password alone preserves the old application's ACL. After
    // replacing an ad-hoc signed build, login could therefore save successfully
    // while the next silent read was still denied. Explicit sign-in rebinds
    // this app-owned record to the current application, never to all apps.
    SecAccessRef access = nullptr;
    if (interaction == Interaction::Allow) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const OSStatus accessStatus = SecAccessCreate(
            CFSTR("VLTONE"), nullptr, &access);
#pragma clang diagnostic pop
        if (accessStatus != errSecSuccess || !access) {
            if (access) CFRelease(access);
            CFRelease(update);
            CFRelease(query);
            CFRelease(data);
            return false;
        }
        CFDictionarySetValue(update, kSecAttrAccess, access);
    }
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        if (access) CFDictionarySetValue(query, kSecAttrAccess, access);
        status = SecItemAdd(query, nullptr);
    }
    if (access) CFRelease(access);
    CFRelease(update);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

QByteArray readNamed(const QString& name, Interaction interaction) {
    if (name == kAccountSlot) return readLocalSession();
    readUnavailable = false;
    KeychainInteractionScope scope(interaction);
    if (!scope.ready()) { readUnavailable = true; return {}; }
    CFMutableDictionaryRef query = baseQuery(name);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) {
        readUnavailable = status != errSecItemNotFound;
        if (result) CFRelease(result);
        return {};
    }
    CFDataRef data = static_cast<CFDataRef>(result);
    QByteArray value(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                     int(CFDataGetLength(data)));
    CFRelease(result);
    return value;
}

bool clearNamed(const QString& name, Interaction interaction) {
    if (name == kAccountSlot) return clearLocalSession();
    KeychainInteractionScope scope(interaction);
    if (!scope.ready()) return false;
    CFMutableDictionaryRef query = baseQuery(name);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else
bool writeNamed(const QString&, const QByteArray&, Interaction) { return false; }
QByteArray readNamed(const QString&, Interaction) { readUnavailable = true; return {}; }
bool clearNamed(const QString&, Interaction) { return true; }
#endif

bool write(const QByteArray& value, Interaction interaction) {
    return writeNamed(kAccountSlot, value, interaction);
}

QByteArray read() { return readNamed(kAccountSlot); }

ReadResult readSession(Interaction interaction) {
    const auto value = readNamed(kAccountSlot, interaction);
    return {value, readUnavailable};
}

bool clear(Interaction interaction) { return clearNamed(kAccountSlot, interaction); }

} // namespace account::securestorage
