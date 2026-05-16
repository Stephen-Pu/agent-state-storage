// LLD §5.1 — PriorityContext.
#include "qos/priority_ctx.h"

namespace kvcache::node::qos {

const char* PriorityName(Priority p) {
    switch (p) {
        case Priority::P0: return "p0";
        case Priority::P1: return "p1";
        case Priority::P2: return "p2";
    }
    return "?";
}

}  // namespace kvcache::node::qos
