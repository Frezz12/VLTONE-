// Compile the real vault implementation against a fake Security API. No test
// operation can read, modify, unlock, or display a dialog for a user's keychain.
#include <Security/Security.h>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
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
#include "../app/SecureStorage.cpp"
#undef SecKeychainGetUserInteractionAllowed
#undef SecKeychainSetUserInteractionAllowed
#undef SecItemCopyMatching
#undef SecItemUpdate
#undef SecItemAdd
#undef SecItemDelete
#undef SecAccessCreate

int main() {
    using namespace account::securestorage;
    check(read() == QByteArray("test"), "authorized startup restores credentials");
    check(interactionAllowed, "read restores the previous policy");

    readStatus = errSecInteractionNotAllowed;
    check(readSession().unavailable, "locked vault is distinguished from missing sign-in without UI");
    check(interactionAllowed, "denied read restores the previous policy");
    readStatus = errSecItemNotFound;
    check(readNamed(QStringLiteral("ai-test")).isEmpty(), "missing API key returns empty");
    check(!readSession().unavailable, "missing session is not a vault access failure");

    check(write(QByteArray("test")), "background token refresh writes silently");
    updateStatus = errSecItemNotFound;
    check(write(QByteArray("test")), "new credential is added silently");
    check(adds == 1 && interactionAllowed, "add restores policy");
    updateStatus = errSecInteractionNotAllowed;
    check(!write(QByteArray("test")) && adds == 1,
          "inaccessible credential must not be replaced or downgraded");

    updateStatus = errSecSuccess;
    expectInteractive = true;
    readStatus = errSecSuccess;
    check(readSession(Interaction::Allow).value == QByteArray("test"), "explicit restore may unlock the existing credential");
    expectAccess = true;
    check(write(QByteArray("test"), Interaction::Allow), "explicit sign-in may request authorization");
    updateStatus = errSecItemNotFound;
    check(write(QByteArray("test"), Interaction::Allow), "new login trusts the current app");
    updateStatus = errSecSuccess;
    accessStatus = errSecNotAvailable;
    const int beforeAccessFailure = vaultCalls;
    check(!write(QByteArray("test"), Interaction::Allow), "ACL creation failure is reported");
    check(vaultCalls == beforeAccessFailure, "failed ACL creation must not modify credentials");
    accessStatus = errSecSuccess;
    check(interactionAllowed, "explicit sign-in preserves policy");
    check(clear(Interaction::Allow), "explicit logout may authorize deletion");
    expectInteractive = false;
    check(clear(), "cleanup does not open dialogs");
    deleteStatus = errSecInteractionNotAllowed;
    check(!clear(), "denied deletion is reported");
    check(interactionAllowed, "failed deletion restores policy");

    interactionAllowed = false;
    const int changes = policyChanges;
    readStatus = errSecSuccess;
    check(read() == QByteArray("test"), "read works with an existing noninteractive policy");
    check(write(QByteArray("test"), Interaction::Allow), "explicit write respects an existing noninteractive policy");
    check(!interactionAllowed && policyChanges == changes, "do not enable UI disabled by another component");
    expectAccess = false;

    interactionAllowed = true;
    int calls = vaultCalls;
    getPolicyStatus = errSecNotAvailable;
    check(read().isEmpty(), "policy lookup failure fails closed");
    check(!write(QByteArray("test")) && !clear(), "mutations fail closed too");
    check(vaultCalls == calls && interactionAllowed, "no vault access if policy cannot be read");
    getPolicyStatus = errSecSuccess;
    setPolicyStatus = errSecNotAvailable;
    check(read().isEmpty() && !write(QByteArray("test")) && !clear(), "policy change failure fails closed");
    check(vaultCalls == calls && interactionAllowed, "no vault access if UI cannot be disabled");
    std::puts("secure_storage_mac_test: PASS");
}
