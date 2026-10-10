#include "numeric_field_checks.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    return numericFieldChecks(app) ? 0 : 1;
}
