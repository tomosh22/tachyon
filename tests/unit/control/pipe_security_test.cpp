// Windows named-pipe DACL of the agent (WP-10 follow-up): an elevated agent must be reachable by the
// interactive user (Authenticated Users) and by nobody else; network clients are rejected.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "control/client.h"
#include "control/server.h"
#include "test_util.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// sddl.h and aclapi.h need windows.h first.
#include <aclapi.h>
#include <sddl.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::control {
namespace {

using testing_util::testAddress;

class StatusHandler final : public IRequestHandler {
 public:
  Response handle(ClientId, const Request&) override {
    StatusBody s;
    s.backend = "pipe-security-test";
    return makeOkResponse(std::nullopt, s);
  }
};

struct Fixture {
  StatusHandler handler;
  std::string address = testAddress("pipesec");
  std::unique_ptr<Server> server;

  Fixture() {
    auto l = listenOn(address);
    EXPECT_TRUE(l.ok());
    server = std::make_unique<Server>(std::move(*l), handler);
    server->start();
  }
};

std::string dacl(const std::string& address) {
  HANDLE h = CreateFileA(address.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return "CreateFile failed: " + std::to_string(GetLastError());
  PSECURITY_DESCRIPTOR sd = nullptr;
  const DWORD rc = GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                   nullptr, &sd);
  CloseHandle(h);
  if (rc != ERROR_SUCCESS) return "GetSecurityInfo failed: " + std::to_string(rc);
  LPSTR text = nullptr;
  std::string result;
  if (ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
                                                           &text, nullptr)) {
    result = text;
    LocalFree(text);
  } else {
    result = "ConvertSecurityDescriptor failed: " + std::to_string(GetLastError());
  }
  LocalFree(sd);
  return result;
}

TEST(PipeSecurity, DaclGrantsAuthenticatedUsersAndDeniesNetworkLogons) {
  Fixture f;
  const std::string sddl = dacl(f.address);
  SCOPED_TRACE(sddl);
  EXPECT_NE(sddl.find("D:"), std::string::npos);
  EXPECT_NE(sddl.find("(D;;"), std::string::npos) << "deny ACE for network logons missing";
  EXPECT_NE(sddl.find(";;;NU)"), std::string::npos);
  EXPECT_NE(sddl.find(";;;AU)"), std::string::npos) << "Authenticated Users must be granted access";
  EXPECT_NE(sddl.find(";;;SY)"), std::string::npos);
  EXPECT_NE(sddl.find(";;;BA)"), std::string::npos);
  // Nothing for Everyone, Anonymous or Guests.
  EXPECT_EQ(sddl.find(";;;WD)"), std::string::npos);
  EXPECT_EQ(sddl.find(";;;AN)"), std::string::npos);
  EXPECT_EQ(sddl.find(";;;BG)"), std::string::npos);
  EXPECT_EQ(sddl.find(";;;LG)"), std::string::npos);
}

// Opens a restricted copy of the current token on which the listed well-known SIDs (and the user SID when
// `disableUser`) are deny-only, so an access check only matches ACEs for the remaining groups.
HANDLE impersonationToken(const std::vector<WELL_KNOWN_SID_TYPE>& disableGroups, bool disableUser) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &token))
    return nullptr;
  std::vector<std::vector<char>> sids;
  std::vector<SID_AND_ATTRIBUTES> disable;
  for (WELL_KNOWN_SID_TYPE type : disableGroups) {
    std::vector<char> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    if (!CreateWellKnownSid(type, nullptr, sid.data(), &size)) {
      CloseHandle(token);
      return nullptr;
    }
    sids.push_back(std::move(sid));
  }
  std::vector<char> userBuf;
  if (disableUser) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    userBuf.resize(size);
    if (!GetTokenInformation(token, TokenUser, userBuf.data(), size, &size)) {
      CloseHandle(token);
      return nullptr;
    }
  }
  for (auto& sid : sids) disable.push_back(SID_AND_ATTRIBUTES{sid.data(), 0});
  if (disableUser)
    disable.push_back(SID_AND_ATTRIBUTES{reinterpret_cast<TOKEN_USER*>(userBuf.data())->User.Sid, 0});

  HANDLE restricted = nullptr;
  const BOOL made = CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, static_cast<DWORD>(disable.size()),
                                          disable.data(), 0, nullptr, 0, nullptr, &restricted);
  CloseHandle(token);
  if (!made) return nullptr;
  HANDLE impersonation = nullptr;
  const BOOL dup = DuplicateTokenEx(restricted, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr,
                                    SecurityImpersonation, TokenImpersonation, &impersonation);
  CloseHandle(restricted);
  return dup ? impersonation : nullptr;
}

// Connects (and round-trips one status request) from a thread that impersonates `token`.
Result<bool> connectAs(HANDLE token, const std::string& address) {
  Result<bool> result = makeError(ErrorCode::Internal, "not run");
  std::thread t([&] {
    if (!SetThreadToken(nullptr, token)) {
      result = makeError(ErrorCode::Internal, "SetThreadToken failed: " + std::to_string(GetLastError()));
      return;
    }
    auto c = Client::connect(address);
    if (!c.ok()) {
      result = c.error();
    } else {
      auto r = c->call(StatusRequest{}, 5000);
      result = r.ok() && r->ok;
    }
    RevertToSelf();
  });
  t.join();
  return result;
}

TEST(PipeSecurity, AnAuthenticatedUserWithoutAdminRightsCanConnectAndTalk) {
  Fixture f;
  // Administrators and the account's own SID are deny-only: only the Authenticated Users ACE can grant
  // access.
  HANDLE token = impersonationToken({WinBuiltinAdministratorsSid}, /*disableUser=*/true);
  ASSERT_NE(token, nullptr) << "CreateRestrictedToken failed: " << GetLastError();
  auto r = connectAs(token, f.address);
  CloseHandle(token);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_TRUE(*r);
}

TEST(PipeSecurity, ACallerWithoutAuthenticatedUsersMembershipIsRejected) {
  Fixture f;
  HANDLE token =
      impersonationToken({WinBuiltinAdministratorsSid, WinAuthenticatedUserSid}, /*disableUser=*/true);
  ASSERT_NE(token, nullptr) << "CreateRestrictedToken failed: " << GetLastError();
  auto r = connectAs(token, f.address);
  CloseHandle(token);
  EXPECT_FALSE(r.ok()) << "a token with no matching ACE must not be able to open the pipe";
}

TEST(PipeSecurity, ASecondListenerOnTheSameNameIsBusy) {
  Fixture f;
  // FILE_FLAG_FIRST_PIPE_INSTANCE: nobody can squat a competing server on a live agent's pipe name.
  auto second = listenOn(f.address);
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code, ErrorCode::Busy);
}

}  // namespace
}  // namespace tachyon::control

#endif  // TACHYON_PLATFORM_WINDOWS
