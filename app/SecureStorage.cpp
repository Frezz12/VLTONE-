#include "SecureStorage.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_MACOS)
#include <Security/Security.h>
#endif

namespace account::securestorage {

namespace {
const QString kAccountSlot = QStringLiteral("desktop-session");
}

#if defined(Q_OS_WIN)
namespace {
std::wstring credentialTarget(const QString& name) {
    if (name == kAccountSlot) return std::wstring(L"VLTStudioPro/DesktopAccount");
    return (QStringLiteral("VLTStudioPro/") + name).toStdWString();
}
}

bool writeNamed(const QString& name, const QByteArray& value) {
    if (value.isEmpty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    const std::wstring target = credentialTarget(name);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = DWORD(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"VLT Studio Pro");
    return CredWriteW(&credential, 0) != FALSE;
}

QByteArray readNamed(const QString& name) {
    const std::wstring target = credentialTarget(name);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    const QByteArray value(reinterpret_cast<const char*>(credential->CredentialBlob),
                           int(credential->CredentialBlobSize));
    CredFree(credential);
    return value;
}

bool clearNamed(const QString& name) {
    const std::wstring target = credentialTarget(name);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE ||
           GetLastError() == ERROR_NOT_FOUND;
}

#elif defined(Q_OS_MACOS)
namespace {
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

bool writeNamed(const QString& name, const QByteArray& value) {
    if (value.isEmpty()) return false;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.constData()), value.size());
    CFMutableDictionaryRef query = baseQuery(name);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(update);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

QByteArray readNamed(const QString& name) {
    CFMutableDictionaryRef query = baseQuery(name);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result) return {};
    CFDataRef data = static_cast<CFDataRef>(result);
    QByteArray value(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                     int(CFDataGetLength(data)));
    CFRelease(result);
    return value;
}

bool clearNamed(const QString& name) {
    CFMutableDictionaryRef query = baseQuery(name);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else
bool writeNamed(const QString&, const QByteArray&) { return false; }
QByteArray readNamed(const QString&) { return {}; }
bool clearNamed(const QString&) { return true; }
#endif

bool write(const QByteArray& value) { return writeNamed(kAccountSlot, value); }

QByteArray read() { return readNamed(kAccountSlot); }

bool clear() { return clearNamed(kAccountSlot); }

} // namespace account::securestorage
