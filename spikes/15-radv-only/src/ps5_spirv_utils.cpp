// Spike 15a: stands in for ANGLE's src/common/spirv/angle_spirv_utils.cpp.
// That file validates and prints SPIR-V with SPIRV-Tools, which the title
// does not link; ANGLE calls both from ASSERTs and debug paths only, which a
// release build (NDEBUG, no ANGLE_ENABLE_ASSERTS) compiles out.
#include "common/spirv/spirv_types.h"

namespace angle
{
namespace spirv
{
bool Validate(const Blob &blob)
{
    return true;
}

void Print(const Blob &blob) {}
}  // namespace spirv
}  // namespace angle
