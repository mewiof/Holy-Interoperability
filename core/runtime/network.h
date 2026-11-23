// Interposers for the host's networking APIs that implement the offline
// switch (see network.cpp).
#pragma once

#include <string_view>

#include "core/runtime/hooks.h"

namespace holy::rt {

const Override* find_network_override(std::string_view library, std::string_view symbol);

}  // namespace holy::rt
