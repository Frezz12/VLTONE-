#include "graphics/GraphicsQualityPolicy.hpp"
#include <iostream>
using namespace ui::graphics;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { if (!ok) { ++failures; std::cerr << label << '\n'; } };
    QualityPolicy policy;
    FrameStats frame; frame.displayHz = 120; frame.renderCpuMs = 7;
    for (int ms = 0; ms < 2000; ms += 100) policy.observe(frame, ms);
    check(policy.level() == QualityLevel::Maximum, "brief load preserves quality");
    policy.observe(frame, 2000);
    check(policy.level() == QualityLevel::Medium, "sustained load reduces quality");
    for (int ms = 2100; ms <= 4200; ms += 100) policy.observe(frame, ms);
    check(policy.level() == QualityLevel::Low, "sustained overload reaches low");
    frame.renderCpuMs = 2;
    for (int ms = 4300; ms <= 14300; ms += 100) policy.observe(frame, ms);
    check(policy.level() == QualityLevel::Medium, "ten seconds of headroom restores one level");
    policy.setQuality(Quality::Maximum);
    frame.renderCpuMs = 100;
    for (int ms = 0; ms < 30000; ms += 100) { ++frame.audioXruns; policy.observe(frame, ms); }
    check(policy.level() == QualityLevel::Maximum, "manual maximum never degrades");
    policy.setQuality(Quality::Automatic); frame.renderCpuMs = 1;
    policy.observe(frame, 0); ++frame.audioXruns; policy.observe(frame, 100);
    check(policy.level() == QualityLevel::Medium, "new xrun promptly reduces background cost");
    frame.active = false; policy.observe(frame, 200);
    frame.active = true; policy.observe(frame, 20000);
    check(policy.level() == QualityLevel::Medium, "hidden time never counts as recovered headroom");
    for (int ms = 20100; ms <= 22200; ms += 100) {
        ++frame.audioXruns;
        policy.observe(frame, ms);
    }
    check(policy.level() == QualityLevel::Low, "repeated xruns reach the floor");
    for (int ms = 22300; ms <= 32300; ms += 100) policy.observe(frame, ms);
    check(policy.level() == QualityLevel::Medium, "xruns at the floor do not prevent later recovery");
    policy.setQuality(Quality::Automatic);
    policy.observe(frame, 0); ++frame.audioXruns; policy.observe(frame, 100);
    frame.active = false; policy.observe(frame, 150);
    frame.active = true; policy.observe(frame, 160);
    ++frame.audioXruns; policy.observe(frame, 170);
    check(policy.level() == QualityLevel::Medium, "briefly hiding does not bypass the two-second switch limit");
    check(QualityPolicy::scale(QualityLevel::Low) == .5 && QualityPolicy::mediaFps(QualityLevel::Medium) == 30,
          "profile dimensions and media rate");
    return failures != 0;
}
