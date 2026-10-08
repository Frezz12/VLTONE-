#include "AudioRuntimeProcess.hpp"
#include "MediaWorker.hpp"
#include "plugins/PluginManager.hpp"

int main(int argc, char** argv) {
    daw::MediaWorker::install(daw::PluginManager::helperPath("daw_worker"));
    return daw::AudioRuntimeProcess::main(argc, argv);
}
