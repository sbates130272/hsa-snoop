#include "aql.h"

namespace aql {

const char* PacketTypeName(PacketType t) {
  switch (t) {
    case PacketType::VendorSpecific: return "vendor_specific";
    case PacketType::Invalid:        return "invalid";
    case PacketType::KernelDispatch: return "kernel_dispatch";
    case PacketType::BarrierAnd:     return "barrier_and";
    case PacketType::AgentDispatch:  return "agent_dispatch";
    case PacketType::BarrierOr:      return "barrier_or";
  }
  return "unknown";
}

}  // namespace aql
