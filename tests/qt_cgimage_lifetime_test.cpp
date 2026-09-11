#if defined(VLT_QT_IMAGE_TEST_DRIVER)
extern "C" int runQtImageLifetimeTest();
int main() { return runQtImageLifetimeTest(); }
#else
#include <QColorSpace>
#include <QImage>

#include <CoreGraphics/CoreGraphics.h>

#include <cstdio>

namespace {
thread_local bool observing = false;
thread_local CGColorSpaceRef createdSpace = nullptr;
thread_local int ownedReferences = 0;
thread_local bool releasedBeforeUse = false;
thread_local bool observedImage = false;

CGColorSpaceRef observeName(CFStringRef name) {
    const auto space = CGColorSpaceCreateWithName(name);
    if (observing) { createdSpace = space; ownedReferences = 1; }
    return space;
}

CGColorSpaceRef observeICC(CFDataRef data) {
    const auto space = CGColorSpaceCreateWithICCData(data);
    if (observing) { createdSpace = space; ownedReferences = 1; }
    return space;
}

CFTypeRef observeRetain(CFTypeRef value) {
    if (observing && value == createdSpace) ++ownedReferences;
    return CFRetain(value);
}

void observeRelease(CFTypeRef value) {
    if (observing && value == createdSpace) --ownedReferences;
    CFRelease(value);
}

CGImageRef observeImage(size_t width, size_t height, size_t bitsPerComponent,
    size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space,
    CGBitmapInfo info, CGDataProviderRef provider, const CGFloat* decode,
    bool interpolate, CGColorRenderingIntent intent) {
    if (observing) observedImage = true;
    if (observing && space == createdSpace && ownedReferences <= 0) {
        releasedBeforeUse = true;
        // Do not pass a dangling pointer on to macOS. Report failure instead
        // of deliberately crashing the test process.
        return nullptr;
    }
    return CGImageCreate(width, height, bitsPerComponent, bitsPerPixel,
        bytesPerRow, space, info, provider, decode, interpolate, intent);
}

// Observe the actual Qt dylib's ownership calls. A cached system colorspace
// can keep the bad pointer alive, so repeated successful conversions alone
// cannot detect this defect. These hooks do not change reference counts.
#define OBSERVE(replacement, original) \
    __attribute__((used, section("__DATA,__interpose"))) \
    const struct { const void* replacement; const void* original; } \
        hook_##original = {reinterpret_cast<const void*>(&replacement), \
                           reinterpret_cast<const void*>(&original)}
OBSERVE(observeName, CGColorSpaceCreateWithName);
OBSERVE(observeICC, CGColorSpaceCreateWithICCData);
OBSERVE(observeRetain, CFRetain);
OBSERVE(observeRelease, CFRelease);
OBSERVE(observeImage, CGImageCreate);
#undef OBSERVE
} // namespace

// No QApplication or display is needed: Cocoa cursors and icons use this same
// conversion. Qt 6.11.0/6.11.1 returned an already-released CGColorSpace from
// qt_mac_cgImageFormatForImage(). Exercise both default sRGB and custom ICC
// profiles, including native image ownership after the QImage is destroyed.
extern "C" int runQtImageLifetimeTest() {
    for (const auto format : {QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA8888}) {
        for (int i = 0; i < 64; ++i) {
            QImage source(32, 32, format);
            source.fill(QColor(30, 80, 120));
            if (i % 2) {
                source.setColorSpace(QColorSpace(QColorSpace::Primaries::SRgb,
                    QColorSpace::TransferFunction::Gamma, 1.8f + float(i) / 1000));
            }
            createdSpace = nullptr;
            ownedReferences = 0;
            releasedBeforeUse = false;
            observedImage = false;
            observing = true;
            CGImageRef image = source.toCGImage();
            observing = false;
            if (releasedBeforeUse) {
                std::fputs("FAIL: Qt released its colorspace before CGImageCreate\n", stderr);
                return 3;
            }
            if (!observedImage) {
                std::fputs("FAIL: native image creation was not observed\n", stderr);
                return 4;
            }
            // The native image must retain both its colorspace and pixel data
            // after the original QImage has gone away.
            source = QImage();
            if (!image) return 1;
            CGColorSpaceRef space = CGImageGetColorSpace(image);
            const bool valid = space &&
                CGColorSpaceGetModel(space) == kCGColorSpaceModelRGB &&
                CGImageGetWidth(image) == 32 && CGImageGetHeight(image) == 32;
            CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(image));
            const bool intact = pixels && CFDataGetLength(pixels) == 32 * 32 * 4;
            if (pixels) CFRelease(pixels);
            CGImageRelease(image);
            if (!valid || !intact) return 2;
        }
    }
    std::puts("Qt native image lifetime: 128 conversions passed");
    return 0;
}
#endif
