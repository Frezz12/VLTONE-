#include "SecureStorage.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#include <mutex>
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
