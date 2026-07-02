#pragma once
#include <string>
#include <vector>
#include "security/boundary_guard.h"
namespace kvcache::node::security {
BoundaryPolicy BuildPolicy(const std::vector<std::string>& allow_rules, bool default_deny = true);
}
