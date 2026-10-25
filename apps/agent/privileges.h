// Elevation check for the capture agent.
#pragma once

namespace tachyon::agent {

// Thread-safe. Windows: process token is elevated. Linux: effective uid 0 or CAP_PERFMON (CapEff bit 38).
bool hasCapturePrivileges();

}  // namespace tachyon::agent
