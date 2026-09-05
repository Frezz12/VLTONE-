#include "ProjectSerializer.hpp"
#include <cstdio>
#include <filesystem>
int main() {
    namespace fs = std::filesystem;
    for (const char* name : {"project.vlt", "Project.vlt", "session.vlt"}) {
        auto path=fs::path("/tmp/vlt-review-20260905/manifests")/name;
        fs::create_directories(path);
        daw::ProjectModel project;
        auto saved=daw::ProjectSerializer::save(project,path.string());
        daw::ProjectModel loaded;
        auto opened=daw::ProjectSerializer::load(loaded,path.string());
        std::printf("%s: save=%d load=%d error=%s\n",name,bool(saved),bool(opened),opened.message().c_str());
    }
    daw::ProjectModel malformed;
    try {
        auto result=daw::ProjectSerializer::deserializeDocument(malformed,R"({"format":7})");
        std::printf("invalid format returns Result=%d\n",bool(result));
    } catch(const std::exception& error) {
        std::printf("invalid format ESCAPED exception: %s\n",error.what());
    }
}
