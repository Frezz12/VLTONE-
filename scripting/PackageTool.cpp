#include "ScriptLibrary.hpp"
#include "VltPackageArchive.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() < 3 || (args[1] != "--check" && args[1] != "--pack")) {
        std::cerr << "vlt_script_tool --check <.lua|.vltcreator|.vltscript|directory>\n"
                     "vlt_script_tool --pack <directory|.vltcreator> <output.vltscript>\n";
        return 2;
    }
    try {
        ui::ScriptPackage package;
        QString error;
        QTemporaryDir staging;
        bool read = false;
        if (QFileInfo(args[2]).suffix() == "vltscript")
            read = staging.isValid() && ui::vltarchive::extract(args[2], staging.path(), error) &&
                   ui::ScriptLibrary::readDirectory(staging.path(), package, error);
        else
            read = ui::ScriptLibrary::read(args[2], package, error);
        if (!read || !ui::ScriptLibrary::validate(package, error)) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }
        if (args[1] == "--pack") {
            if (args.size() != 4)
                return 2;
            if (!ui::ScriptLibrary::exportPackage(package, args[3], error)) {
                std::cerr << error.toStdString() << '\n';
                return 1;
            }
        }
        std::cout << vlt::script::Json{{"valid", true},
                                       {"sdk", 1},
                                       {"id", package.id().toStdString()},
                                       {"sources", package.sources.size()}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
