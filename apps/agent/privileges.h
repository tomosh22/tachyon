// Elevation check for the capture agent.
#pragma once

#include "control/transport.h"

#include <cstdint>
#include <string>

namespace tachyon::agent {

// Thread-safe. Windows: process token is elevated. Linux: effective uid 0, CAP_PERFMON (CapEff bit 38) or
// CAP_SYS_ADMIN (bit 21, kernels before 5.8), or kernel.perf_event_paranoid <= 0.
bool hasCapturePrivileges();

// WP-41b. Thread-safe: pure function. Parses a decimal uid as given to --socket-owner; false for anything but
// 0 < uid < 2^32 - 1 (the agent never hands its sockets to root).
bool parseSocketOwnerUid(const std::string& text, uint32_t& uid);

// WP-41b. Thread-safe: pure function. The user an elevated agent was started for: when `isRoot`, the uid in
// PKEXEC_UID, else in SUDO_UID (null pointers: variable not set), provided it is a valid non-zero uid.
// Returns false when there is none.
bool socketOwnerFromEnvironment(bool isRoot, const char* pkexecUid, const char* sudoUid, uint32_t& uid);

// WP-41b. Thread-safe: reads the account database (getpwuid_r; on Windows: nothing). The SocketOwner for
// `uid` with its primary group; the gid stays -1 (unchanged) when the account is unknown.
control::SocketOwner socketOwnerForUid(uint32_t uid);

}  // namespace tachyon::agent
