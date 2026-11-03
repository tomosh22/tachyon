// Elevation check for the capture agent.
#pragma once

namespace tachyon::agent {

// Thread-safe. Windows: process token is elevated. Linux: effective uid 0, CAP_PERFMON (CapEff bit 38) or
// CAP_SYS_ADMIN (bit 21, kernels before 5.8), or kernel.perf_event_paranoid <= 0.
bool hasCapturePrivileges();

}  // namespace tachyon::agent
