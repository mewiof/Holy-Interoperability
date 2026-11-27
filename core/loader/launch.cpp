#include "core/loader/launch.h"
#include "core/loader/profiler.h"

#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include "core/common/boot_block.h"
#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/common/vfs.h"
#include "core/package/manifest.h"
#include "core/pe/image.h"
#include "core/pe/mapped_file.h"

namespace holy::loader {
namespace {

constexpr char kComponent[] = "launch";

struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::string last_error_text(const char* what) {
  return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// --- command line and environment -----------------------------------------

void append_quoted(std::wstring& out, const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    out.append(argument);
    return;
  }
  out.push_back(L'"');
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < argument.size() && argument[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == argument.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(argument[i]);
  }
  out.push_back(L'"');
}

std::vector<wchar_t> build_environment(const std::map<std::wstring, std::wstring>& overrides) {
  std::vector<std::pair<std::wstring, std::wstring>> variables;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
      const std::wstring_view view(entry);
      // Entries such as "=C:=C:\dir" start with '=' and must be preserved as-is.
      const size_t equals = view.find(L'=', 1);
      if (equals == std::wstring_view::npos) continue;
      variables.emplace_back(std::wstring(view.substr(0, equals)), std::wstring(view.substr(equals + 1)));
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [name, value] : overrides) {
    bool replaced = false;
    for (auto& variable : variables) {
      if (iequals(variable.first, name)) {
        variable.second = value;
        replaced = true;
      }
    }
    if (!replaced) variables.emplace_back(name, value);
  }
  std::vector<wchar_t> out;
  for (const auto& [name, value] : variables) {
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(L'=');
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

// --- sandbox ----------------------------------------------------------------

// Duplicates the current process token and lowers its integrity level. A
// process started with it cannot modify objects labelled medium or higher,
// which covers the title package and practically all user data.
bool create_low_integrity_token(Handle& out, std::string& error) {
  Handle current;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
                        &current.value)) {
    error = last_error_text("cannot open the process token");
    return false;
  }
  if (!DuplicateTokenEx(current.value, 0, nullptr, SecurityImpersonation, TokenPrimary, &out.value)) {
    error = last_error_text("cannot duplicate the process token");
    return false;
  }
  PSID low_sid = nullptr;
  if (!ConvertStringSidToSidW(L"S-1-16-4096", &low_sid)) {
    error = last_error_text("cannot build the low integrity SID");
    return false;
  }
  TOKEN_MANDATORY_LABEL label{};
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  label.Label.Sid = low_sid;
  const BOOL ok = SetTokenInformation(out.value, TokenIntegrityLevel, &label,
                                      static_cast<DWORD>(sizeof(label) + GetLengthSid(low_sid)));
  if (!ok) error = last_error_text("cannot lower the token integrity level");
  LocalFree(low_sid);
  return ok != 0;
}

// --- remote image access ----------------------------------------------------

struct Remote {
  HANDLE process;

  bool read(uint64_t address, void* buffer, size_t size) const {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &done) &&
           done == size;
  }
  bool write(uint64_t address, const void* buffer, size_t size) const {
    SIZE_T done = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), buffer, size, &done) &&
           done == size;
  }
};

bool query_image_base(const Remote& remote, uint64_t& image_base, std::string& error) {
  PROCESS_BASIC_INFORMATION info{};
  const NTSTATUS status =
      NtQueryInformationProcess(remote.process, ProcessBasicInformation, &info, sizeof(info), nullptr);
  if (status < 0 || !info.PebBaseAddress) {
    error = "cannot locate the process environment block of the title process";
    return false;
  }
  // PEB::ImageBaseAddress sits at offset 0x10 in 64-bit processes.
  if (!remote.read(reinterpret_cast<uint64_t>(info.PebBaseAddress) + 0x10, &image_base, sizeof(image_base))) {
    error = last_error_text("cannot read the image base of the title process");
    return false;
  }
  return true;
}

// Import directory entries are 32-bit offsets from the image base, so the
// replacement table has to live above the image and within 4 GiB of its base.
uint64_t allocate_above_image(HANDLE process, uint64_t image_base, uint32_t image_size, size_t size) {
  constexpr uint64_t kGranularity = 0x10000;
  constexpr uint64_t kReach = 0xF0000000ull;
  uint64_t address = align_up(image_base + image_size, kGranularity);
  while (address + size < image_base + kReach) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) break;
    const uint64_t region_end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
    if (info.State == MEM_FREE && region_end - address >= size) {
      void* block = VirtualAllocEx(process, reinterpret_cast<LPVOID>(address), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
      if (block) return reinterpret_cast<uint64_t>(block);
    }
    address = align_up(region_end, kGranularity);
  }
  return 0;
}

// Points the suspended process's import directory at a one-entry table that
// imports the runtime DLL, and records where the original directory was.
bool take_over_imports(HANDLE process, const std::string& runtime_dll_ansi, BootBlock block,
                       std::string& error) {
  const Remote remote{process};
  uint64_t image_base = 0;
  if (!query_image_base(remote, image_base, error)) return false;

  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!remote.read(image_base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew <= 0) {
    error = "the title process image has no valid DOS header";
    return false;
  }
  const uint64_t nt_address = image_base + static_cast<uint32_t>(dos.e_lfanew);
  // The whole directory array is rewritten below, so insist on the standard
  // full-size optional header rather than risk writing past a shorter one.
  if (!remote.read(nt_address, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
      nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
    error = "the title process image has no valid PE32+ header";
    return false;
  }

  IMAGE_DATA_DIRECTORY* directories = nt.OptionalHeader.DataDirectory;
  block.original_import_rva = directories[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  block.original_import_size = directories[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
  block.original_iat_rva = directories[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
  block.original_iat_size = directories[IMAGE_DIRECTORY_ENTRY_IAT].Size;

  const size_t off_descriptors = kBootDescriptorOffset;
  const size_t off_lookup = off_descriptors + 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
  const size_t off_address = off_lookup + 2 * sizeof(uint64_t);
  const size_t off_hint_name = off_address + 2 * sizeof(uint64_t);
  const size_t off_dll_name = align_up(off_hint_name + sizeof(uint16_t) + sizeof(kBootAnchorSymbol), 2);
  const size_t total = align_up(off_dll_name + runtime_dll_ansi.size() + 1, 0x1000);

  const uint64_t remote_block =
      allocate_above_image(process, image_base, nt.OptionalHeader.SizeOfImage, total);
  if (!remote_block) {
    error = "cannot allocate memory near the title image";
    return false;
  }
  const uint64_t block_rva = remote_block - image_base;
  const auto rva = [&](size_t offset) { return static_cast<uint32_t>(block_rva + offset); };

  std::vector<uint8_t> buffer(total, 0);
  std::memcpy(buffer.data(), &block, sizeof(block));

  IMAGE_IMPORT_DESCRIPTOR descriptor{};
  descriptor.OriginalFirstThunk = rva(off_lookup);
  descriptor.Name = rva(off_dll_name);
  descriptor.FirstThunk = rva(off_address);
  std::memcpy(buffer.data() + off_descriptors, &descriptor, sizeof(descriptor));

  const uint64_t thunk = rva(off_hint_name);
  std::memcpy(buffer.data() + off_lookup, &thunk, sizeof(thunk));
  std::memcpy(buffer.data() + off_address, &thunk, sizeof(thunk));
  std::memcpy(buffer.data() + off_hint_name + sizeof(uint16_t), kBootAnchorSymbol, sizeof(kBootAnchorSymbol));
  std::memcpy(buffer.data() + off_dll_name, runtime_dll_ansi.data(), runtime_dll_ansi.size());

  if (!remote.write(remote_block, buffer.data(), buffer.size())) {
    error = last_error_text("cannot write the boot block into the title process");
    return false;
  }

  IMAGE_DATA_DIRECTORY patched[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
  std::memcpy(patched, directories, sizeof(patched));
  patched[IMAGE_DIRECTORY_ENTRY_IMPORT] = {rva(off_descriptors), 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
  patched[IMAGE_DIRECTORY_ENTRY_IAT] = {rva(off_address), 2 * sizeof(uint64_t)};
  patched[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {0, 0};

  const uint64_t directories_address =
      nt_address + offsetof(IMAGE_NT_HEADERS64, OptionalHeader.DataDirectory);
  DWORD old_protection = 0;
  if (!VirtualProtectEx(process, reinterpret_cast<LPVOID>(directories_address), sizeof(patched),
                        PAGE_READWRITE, &old_protection)) {
    error = last_error_text("cannot unprotect the title image headers");
    return false;
  }
  const bool written = remote.write(directories_address, patched, sizeof(patched));
  VirtualProtectEx(process, reinterpret_cast<LPVOID>(directories_address), sizeof(patched), old_protection,
                   &old_protection);
  if (!written) {
    error = last_error_text("cannot redirect the title import directory");
    return false;
  }
  return true;
}

// The PE import table stores library names in the system ANSI code page.
bool to_import_name(const std::wstring& path, std::string& out) {
  const auto convert = [&](const std::wstring& text) {
    BOOL lossy = FALSE;
    const int needed =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), -1, nullptr, 0, nullptr, &lossy);
    if (needed <= 1 || lossy) return false;
    out.resize(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), -1, out.data(), needed, nullptr, &lossy);
    out.resize(static_cast<size_t>(needed) - 1);
    return !lossy;
  };
  if (convert(path)) return true;
  wchar_t short_path[MAX_PATH];
  const DWORD length = GetShortPathNameW(path.c_str(), short_path, MAX_PATH);
  return length > 0 && length < MAX_PATH && convert(short_path);
}

bool copy_to(wchar_t* destination, size_t capacity, const std::wstring& text) {
  if (text.size() >= capacity) return false;
  std::memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  return true;
}

HANDLE inheritable_std_handle(DWORD which, std::vector<HANDLE>& owned) {
  const HANDLE source = GetStdHandle(which);
  if (source == nullptr || source == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    return nullptr;
  }
  owned.push_back(duplicate);
  return duplicate;
}

}  // namespace

std::wstring default_data_root() {
  std::wstring root;
  PWSTR folder = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, KF_FLAG_CREATE, nullptr, &folder)) && folder) {
    root = join_path(folder, L"HolyInterop");
  }
  if (folder) CoTaskMemFree(folder);
  return root;
}

LaunchResult launch(const LaunchOptions& options) {
  LaunchResult result;
  const auto fail = [&](std::string message) {
    result.status = LaunchStatus::Failed;
    result.error = std::move(message);
    return result;
  };

  std::string error;
  const auto manifest = package::load(options.package_dir, &error);
  if (!manifest) return fail(error);

  std::wstring exe_name = options.executable;
  if (exe_name.empty()) {
    const package::Executable* exe = manifest->default_executable();
    if (!exe) return fail("the manifest lists no executable");
    exe_name = widen(exe->name);
  }
  const std::wstring exe_path = absolute_path(join_path(manifest->root, exe_name));
  if (!vfs::is_inside(manifest->root, exe_path) || !file_exists(exe_path)) {
    return fail("the executable named by the manifest does not exist inside the package");
  }

  {
    auto file = pe::MappedFile::open(exe_path, &error);
    if (!file) return fail("cannot read the title executable: " + error);
    const auto image = pe::Image::parse(file->bytes(), pe::Layout::File, &error);
    if (!image) return fail("the title executable is not supported: " + error);
    if (image->is_dll()) return fail("the title executable is a DLL");
  }

  std::wstring runtime_dll = options.runtime_dll;
  if (runtime_dll.empty()) runtime_dll = join_path(parent_dir(module_path(nullptr)), L"holyrt.dll");
  runtime_dll = absolute_path(runtime_dll);
  if (!file_exists(runtime_dll)) return fail("holyrt.dll was not found next to the launcher");
  std::string runtime_dll_ansi;
  if (!to_import_name(runtime_dll, runtime_dll_ansi)) {
    return fail("the runtime path cannot be represented in the system code page; install to a simpler path");
  }

  std::wstring data_dir = options.data_dir;
  if (data_dir.empty()) {
    const std::wstring root = default_data_root();
    if (root.empty()) return fail("cannot determine the per-user data directory");
    data_dir = join_path(join_path(root, L"titles"), widen(manifest->storage_key()));
  }
  data_dir = absolute_path(data_dir);
  for (const wchar_t* sub : {L"", L"logs", L"temp", L"scratch"}) {
    if (!create_directories(join_path(data_dir, sub))) return fail("cannot create the title data directory");
  }
  result.data_dir = data_dir;
  result.log_path = runtime_log_path(data_dir);

  BootBlock block{};
  block.magic = kBootMagic;
  block.version = kBootVersion;
  block.struct_size = sizeof(BootBlock);
  block.flags = (options.unimplemented == UnimplementedPolicy::Trap ? kBootTrapUnimplemented : 0u) |
                (options.sandbox ? kBootSandboxed : 0u) | (options.offline ? kBootOffline : 0u) |
                (options.local_user ? kBootLocalUser : 0u);
  block.log_level = static_cast<uint32_t>(options.runtime_log_level);
  block.console_model = static_cast<uint32_t>(options.console_model);
  if (!copy_to(block.package_dir, std::size(block.package_dir), manifest->root) ||
      !copy_to(block.data_dir, std::size(block.data_dir), data_dir) ||
      !copy_to(block.trace_filter, std::size(block.trace_filter), options.trace_filter)) {
    return fail("a path or option is too long for the boot block");
  }

  std::wstring command_line;
  append_quoted(command_line, exe_path);
  for (const std::wstring& argument : options.title_args) {
    command_line.push_back(L' ');
    append_quoted(command_line, argument);
  }

  const std::wstring temp_dir = join_path(data_dir, L"temp");
  // The title process's user profile path points at its host cache
  // directory, so the graphics driver and runtime keep their shader caches
  // there instead of in the user's shared ones. A run in which the host
  // refused a pipeline leaves a marker; such caches are discarded, because
  // the host driver has been seen to fail on its own cache entries for a
  // refused pipeline in every later process.
  const std::wstring host_dir = join_path(data_dir, kHostCacheDir);
  if (file_exists(join_path(host_dir, kHostCacheDiscard))) {
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(host_dir), ignored);
    LOG_INFO(kComponent, "host caches discarded after a refused pipeline");
  }
  if (!create_directories(host_dir)) return fail("cannot create the host cache directory");
  std::vector<wchar_t> environment =
      build_environment({{L"TEMP", temp_dir}, {L"TMP", temp_dir}, {L"USERPROFILE", host_dir}});

  std::vector<HANDLE> inherited;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = inheritable_std_handle(STD_INPUT_HANDLE, inherited);
  startup.hStdOutput = inheritable_std_handle(STD_OUTPUT_HANDLE, inherited);
  startup.hStdError = inheritable_std_handle(STD_ERROR_HANDLE, inherited);
  const auto close_inherited = [&] {
    for (HANDLE handle : inherited) CloseHandle(handle);
    inherited.clear();
  };

  // The title's current directory is the package root, as on the console.
  const DWORD creation_flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  PROCESS_INFORMATION process_info{};
  BOOL created = FALSE;
  if (options.sandbox) {
    Handle token;
    if (!create_low_integrity_token(token, error)) {
      close_inherited();
      return fail("cannot set up the sandbox: " + error);
    }
    created = CreateProcessAsUserW(token.value, exe_path.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                   creation_flags, environment.data(), manifest->root.c_str(), &startup,
                                   &process_info);
  } else {
    created = CreateProcessW(exe_path.c_str(), command_line.data(), nullptr, nullptr, TRUE, creation_flags,
                             environment.data(), manifest->root.c_str(), &startup, &process_info);
  }
  const std::string create_error =
      created ? std::string() : last_error_text("cannot create the title process");
  close_inherited();
  if (!created) return fail(create_error);

  const Handle process(process_info.hProcess);
  const Handle thread(process_info.hThread);

  // Tie the title's lifetime to the launcher.
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job.value, process.value);
  }

  if (!take_over_imports(process.value, runtime_dll_ansi, block, error)) {
    TerminateProcess(process.value, kExitRuntimeFailure);
    return fail(error);
  }

  LOG_DEBUG(kComponent, "title process %lu created%s", process_info.dwProcessId,
            options.sandbox ? " (low integrity)" : "");
  ResumeThread(thread.value);

  std::unique_ptr<Profiler> profiler;
  if (options.profile_interval_ms) {
    profiler = std::make_unique<Profiler>(process.value, options.profile_interval_ms,
                                          join_path(join_path(data_dir, L"logs"), L"profile.txt"), exe_path);
  }
  const DWORD wait = WaitForSingleObject(process.value, options.timeout_ms ? options.timeout_ms : INFINITE);
  profiler.reset();  // the final report
  if (wait == WAIT_TIMEOUT) {
    if (job) TerminateJobObject(job.value, 1);
    TerminateProcess(process.value, 1);
    WaitForSingleObject(process.value, 5000);
    result.status = LaunchStatus::TimedOut;
    return result;
  }
  DWORD exit_code = 0;
  GetExitCodeProcess(process.value, &exit_code);
  result.status = LaunchStatus::Exited;
  result.exit_code = exit_code;
  return result;
}

}  // namespace holy::loader
