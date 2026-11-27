#include "x86/lifted.h"

extern "C" __attribute__((visibility("default"))) void darkest_install_code() {
    recomp::RegisterLiftedFunctions(recomp::kLiftedFunctions, recomp::kLiftedFunctionCount);
}
