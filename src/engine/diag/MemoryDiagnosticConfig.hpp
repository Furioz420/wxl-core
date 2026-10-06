#pragma once
#include "common/Config.hpp"
namespace wxl::diag {
inline bool MemoryOwnersEnabled() {
#ifdef WXL_DIAGNOSTIC_MEMORY_OWNERS
    constexpr bool defaultValue = true;
#else
    constexpr bool defaultValue = false;
#endif
    return wxl::config::Env("WXL_MEMORY_OWNERS", defaultValue);
}
}
