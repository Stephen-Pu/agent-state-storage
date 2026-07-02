#include "security/boundary_policy_builder.h"
namespace kvcache::node::security {
namespace {
// A token is CIDR if it has a '/' and the part after it is all digits.
bool LooksLikeCidr(const std::string& s) {
    auto slash = s.find('/');
    if (slash == std::string::npos || slash + 1 >= s.size()) return false;
    for (size_t i = slash + 1; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
}  // namespace
BoundaryPolicy BuildPolicy(const std::vector<std::string>& allow_rules, bool default_deny) {
    BoundaryPolicy p; p.default_deny = default_deny;
    for (const auto& tok : allow_rules) {
        if (tok.empty()) continue;
        Rule r;
        if (LooksLikeCidr(tok)) r.cidr = tok; else r.host_glob = tok;
        p.allow.push_back(std::move(r));
    }
    return p;
}
}  // namespace kvcache::node::security
