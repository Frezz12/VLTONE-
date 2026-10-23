#include "RenderWorker.hpp"
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Module.hpp"
#endif

int main(int argc, char** argv) {
    const int result = daw::RenderWorker::main(argc, argv);
#if DAW_ENABLE_VST3
    daw::plugins::Vst3Module::releaseUnusedModules();
#endif
    return result;
}
