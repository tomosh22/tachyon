// Linked into sdk_tests (the WP-50 behaviour tests). They exercise the API on the process-wide runtime, so
// give it a harmless configuration before main(): no agent, and a sidecar file in the working directory with
// a fixed name instead of one per pid. (The runtime reads the environment lazily, on the first API call.)
#include "test_env.h"

namespace {
struct SdkTestEnvironment {
  SdkTestEnvironment() {
    tachyon::sdk_testing::setEnv("TACHYON_AGENT_PIPE", "no-such-agent-for-sdk-tests");
    tachyon::sdk_testing::setEnv("TACHYON_SIDECAR", "sdk_tests.tachyon-inst");
    tachyon::sdk_testing::unsetEnv("TACHYON_DISABLE");
  }
};
const SdkTestEnvironment g_environment;
}  // namespace
