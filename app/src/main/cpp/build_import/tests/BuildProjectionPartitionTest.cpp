#include "../BuildProjectionRuntime.h"

#include <iostream>
#include <string>

int main() {
    std::string error;
    if (!build_import::RunBuildProjectionPartitionSpoolSelfTest(".", &error)) {
        std::cerr << "BuildProjectionPartitionTest failed: " << error << '\n';
        return 1;
    }
    std::cout << "BuildProjectionPartitionTest passed\n";
    return 0;
}
