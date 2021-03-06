// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/process_metric.h"

#include <memory>
#include <utility>

#include "base/optional.h"

#if defined(OS_WIN)
#include <windows.h>

#include <psapi.h>
#include "base/win/win_util.h"
#endif

#if defined(OS_MACOSX)
#include <mach/mach.h>
#include "base/process/port_provider_mac.h"
#include "content/public/browser/browser_child_process_host.h"

extern "C" int sandbox_check(pid_t pid, const char* operation, int type, ...);

namespace {

mach_port_t TaskForPid(pid_t pid) {
  mach_port_t task = MACH_PORT_NULL;
  if (auto* port_provider = content::BrowserChildProcessHost::GetPortProvider())
    task = port_provider->TaskForPid(pid);
  if (task == MACH_PORT_NULL && pid == getpid())
    task = mach_task_self();
  return task;
}

base::Optional<mach_task_basic_info_data_t> GetTaskInfo(mach_port_t task) {
  if (task == MACH_PORT_NULL)
    return base::nullopt;
  mach_task_basic_info_data_t info = {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t kr = task_info(task, MACH_TASK_BASIC_INFO,
                               reinterpret_cast<task_info_t>(&info), &count);
  return (kr == KERN_SUCCESS) ? base::make_optional(info) : base::nullopt;
}

}  // namespace

#endif  // defined(OS_MACOSX)

namespace electron {

ProcessMetric::ProcessMetric(int type,
                             base::ProcessHandle handle,
                             std::unique_ptr<base::ProcessMetrics> metrics) {
  this->type = type;
  this->metrics = std::move(metrics);

#if defined(OS_WIN)
  HANDLE duplicate_handle = INVALID_HANDLE_VALUE;
  ::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(),
                    &duplicate_handle, 0, false, DUPLICATE_SAME_ACCESS);
  this->process = base::Process(duplicate_handle);
#else
  this->process = base::Process(handle);
#endif
}

ProcessMetric::~ProcessMetric() = default;

#if defined(OS_WIN)

ProcessMemoryInfo ProcessMetric::GetMemoryInfo() const {
  ProcessMemoryInfo result;

  PROCESS_MEMORY_COUNTERS_EX info = {};
  if (::GetProcessMemoryInfo(process.Handle(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                             sizeof(info))) {
    result.working_set_size = info.WorkingSetSize;
    result.peak_working_set_size = info.PeakWorkingSetSize;
    result.private_bytes = info.PrivateUsage;
  }

  return result;
}

ProcessIntegrityLevel ProcessMetric::GetIntegrityLevel() const {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(process.Handle(), TOKEN_QUERY, &token)) {
    return ProcessIntegrityLevel::Unknown;
  }

  base::win::ScopedHandle token_scoped(token);

  DWORD token_info_length = 0;
  if (::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0,
                            &token_info_length) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return ProcessIntegrityLevel::Unknown;
  }

  auto token_label_bytes = std::make_unique<char[]>(token_info_length);
  auto* token_label =
      reinterpret_cast<TOKEN_MANDATORY_LABEL*>(token_label_bytes.get());
  if (!::GetTokenInformation(token, TokenIntegrityLevel, token_label,
                             token_info_length, &token_info_length)) {
    return ProcessIntegrityLevel::Unknown;
  }

  DWORD integrity_level = *::GetSidSubAuthority(
      token_label->Label.Sid,
      static_cast<DWORD>(*::GetSidSubAuthorityCount(token_label->Label.Sid) -
                         1));

  if (integrity_level >= SECURITY_MANDATORY_UNTRUSTED_RID &&
      integrity_level < SECURITY_MANDATORY_LOW_RID) {
    return ProcessIntegrityLevel::Untrusted;
  }

  if (integrity_level >= SECURITY_MANDATORY_LOW_RID &&
      integrity_level < SECURITY_MANDATORY_MEDIUM_RID) {
    return ProcessIntegrityLevel::Low;
  }

  if (integrity_level >= SECURITY_MANDATORY_MEDIUM_RID &&
      integrity_level < SECURITY_MANDATORY_HIGH_RID) {
    return ProcessIntegrityLevel::Medium;
  }

  if (integrity_level >= SECURITY_MANDATORY_HIGH_RID &&
      integrity_level < SECURITY_MANDATORY_SYSTEM_RID) {
    return ProcessIntegrityLevel::High;
  }

  return ProcessIntegrityLevel::Unknown;
}

// static
bool ProcessMetric::IsSandboxed(ProcessIntegrityLevel integrity_level) {
  return integrity_level > ProcessIntegrityLevel::Unknown &&
         integrity_level < ProcessIntegrityLevel::Medium;
}

#elif defined(OS_MACOSX)

ProcessMemoryInfo ProcessMetric::GetMemoryInfo() const {
  ProcessMemoryInfo result;

  if (auto info = GetTaskInfo(TaskForPid(process.Pid()))) {
    result.working_set_size = info->resident_size;
    result.peak_working_set_size = info->resident_size_max;
  }

  return result;
}

bool ProcessMetric::IsSandboxed() const {
#if defined(MAS_BUILD)
  return true;
#else
  return sandbox_check(process.Pid(), nullptr, 0) != 0;
#endif
}

#endif  // defined(OS_MACOSX)

}  // namespace electron
