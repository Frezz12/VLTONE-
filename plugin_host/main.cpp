#include "PluginProcess.hpp"
#include <cstdlib>
#ifdef DAW_PLUGIN_HOST_GUI
int runPluginHostGui(int argc, char** argv);
#endif

int main(int argc, char** argv) {
    // The server tears down the instance explicitly. Do not let unrelated SDK
    // statics or the POSIX parent-death watcher run through CRT teardown.
#ifdef DAW_PLUGIN_HOST_GUI
    const int result = runPluginHostGui(argc, argv);
#else
    const int result = daw::plugins::runPluginHost(argc, argv);
#endif
    std::_Exit(result);
}
