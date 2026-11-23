// holy: command line front end.
//
//   holy inspect <package-dir> [--all]
//   holy run <package-dir> [options] [-- title arguments]
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/analysis/import_report.h"
#include "core/common/boot_block.h"
#include "core/common/log.h"
#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/loader/launch.h"
#include "core/package/manifest.h"
#include "core/pe/image.h"
#include "core/pe/mapped_file.h"
#include "core/runtime/api.h"

namespace {

using namespace holy;

constexpr char kVersion[] = "0.1.0";

enum ExitCode { kOk = 0, kTitleFailed = 1, kUsage = 2, kLaunchFailed = 3, kTimedOut = 4, kUnimplemented = 5 };

void print_usage() {
  std::puts(
      "holy - run x64 console title packages as native Windows processes\n"
      "\n"
      "Usage:\n"
      "  holy inspect <package-dir> [--all]\n"
      "      Report how the title's imports would bind, without running anything.\n"
      "      --all                 also report every DLL in the package root\n"
      "\n"
      "  holy run <package-dir> [options] [-- <title arguments>]\n"
      "      Start the title from its package directory. The package is never modified.\n"
      "      --exe <name>          executable inside the package (default: from the manifest)\n"
      "      --data-dir <dir>      per-title writable directory (default: per-user, per-title)\n"
      "      --no-sandbox          do not lower the title's integrity level\n"
      "      --console <model>     console model reported to the title: series-x (default), series-s\n"
      "      --network <mode>      offline (default): the title sees a disconnected host;\n"
      "                            host: the title may use the host's network\n"
      "      --unimplemented <trap|continue>\n"
      "                            what a call into an unimplemented import does (default: trap)\n"
      "      --trace <patterns>    comma separated import library patterns to trace, e.g. xmem.dll,d3d12*\n"
      "      --log-level <level>   trace, debug, info, warn, error (default: info)\n"
      "      --timeout <seconds>   stop the title after this long\n"
      "\n"
      "  holy version\n");
}

std::wstring libs_dir() {
  return join_path(parent_dir(module_path(nullptr)), L"libs");
}

const char* subsystem_name(uint16_t subsystem) {
  switch (subsystem) {
    case IMAGE_SUBSYSTEM_WINDOWS_GUI:
      return "GUI";
    case IMAGE_SUBSYSTEM_WINDOWS_CUI:
      return "console";
    default:
      return "other";
  }
}

bool inspect_image(const std::wstring& path, const ProviderContext& context, bool list_host_libraries) {
  std::string error;
  auto file = pe::MappedFile::open(path, &error);
  if (!file) {
    std::printf("  cannot read: %s\n", error.c_str());
    return false;
  }
  const auto image = pe::Image::parse(file->bytes(), pe::Layout::File, &error);
  if (!image) {
    std::printf("  unsupported image: %s\n", error.c_str());
    return false;
  }

  std::printf("  x64 %s, %s subsystem, %.1f MiB mapped, %s, %s\n", image->is_dll() ? "DLL" : "executable",
              subsystem_name(image->subsystem()), image->size_of_image() / (1024.0 * 1024.0),
              image->has_relocations() ? "relocatable" : "fixed base",
              image->tls() ? "static TLS" : "no static TLS");

  const analysis::ImageReport report =
      analysis::check_imports(*image, context, [](std::string_view library, std::string_view symbol) {
        return HolyHasOverride(std::string(library).c_str(), std::string(symbol).c_str()) != 0;
      });

  size_t host_libraries = 0, host_symbols = 0, host_resolved = 0;
  std::printf("\n  %-8s %-52s %s\n", "PROVIDER", "LIBRARY", "RESOLVED");
  for (const analysis::LibraryReport& library : report.libraries) {
    const bool complete = library.resolved == library.total;
    if (library.kind == ProviderKind::Host && complete && !list_host_libraries) {
      ++host_libraries;
      host_symbols += library.total;
      host_resolved += library.resolved;
      continue;
    }
    std::printf("  %-8s %-52s %zu/%zu%s%s\n", provider_kind_name(library.kind), library.name.c_str(),
                library.resolved, library.total, library.delay_loaded ? " (delay-loaded)" : "",
                library.found || library.kind == ProviderKind::Missing ? "" : " (not found)");
  }
  if (host_libraries) {
    std::printf("  %-8s %-52s %zu/%zu\n", "host",
                ("(" + std::to_string(host_libraries) + " fully resolved libraries)").c_str(), host_resolved,
                host_symbols);
  }

  size_t unresolved = 0;
  for (const analysis::LibraryReport& library : report.libraries) unresolved += library.unresolved.size();
  if (unresolved) {
    std::printf("\n  Unresolved imports (%zu):\n", unresolved);
    for (const analysis::LibraryReport& library : report.libraries) {
      for (const std::string& symbol : library.unresolved) {
        std::printf("    %s!%s\n", library.name.c_str(), symbol.c_str());
      }
    }
  }

  std::printf("\n  Imports resolved: %zu of %zu\n", report.resolved, report.total);
  if (report.console_total) {
    std::printf(
        "  Console library coverage: %zu of %zu symbols (%.0f%%)\n", report.console_resolved,
        report.console_total,
        100.0 * static_cast<double>(report.console_resolved) / static_cast<double>(report.console_total));
  }
  return true;
}

int cmd_inspect(const std::vector<std::wstring>& args) {
  std::wstring package_dir;
  bool all = false;
  for (const std::wstring& arg : args) {
    if (arg == L"--all")
      all = true;
    else if (!arg.empty() && arg[0] == L'-') {
      std::fprintf(stderr, "unknown option: %s\n", narrow(arg).c_str());
      return kUsage;
    } else if (package_dir.empty())
      package_dir = arg;
    else
      return kUsage;
  }
  if (package_dir.empty()) {
    print_usage();
    return kUsage;
  }

  std::string error;
  const auto manifest = package::load(package_dir, &error);
  if (!manifest) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return kLaunchFailed;
  }

  std::printf("Package\n");
  std::printf("  identity   %s %s\n", manifest->identity_name.c_str(), manifest->identity_version.c_str());
  std::printf("  data key   %s\n", manifest->storage_key().c_str());
  for (const package::Executable& exe : manifest->executables) {
    std::printf("  executable %s (%s%s)\n", exe.name.c_str(),
                exe.target_device_family.empty() ? "unspecified family" : exe.target_device_family.c_str(),
                exe.dev_only ? ", dev only" : "");
  }

  const ProviderContext context{libs_dir(), manifest->root};
  const package::Executable* main_exe = manifest->default_executable();
  bool ok = true;
  if (main_exe) {
    std::printf("\nExecutable %s\n", main_exe->name.c_str());
    ok = inspect_image(join_path(manifest->root, widen(main_exe->name)), context, false);
  }

  if (all) {
    WIN32_FIND_DATAW entry;
    const HANDLE find = FindFirstFileW(join_path(manifest->root, L"*.dll").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::printf("\nPackage library %s\n", narrow(entry.cFileName).c_str());
        inspect_image(join_path(manifest->root, entry.cFileName), context, false);
      } while (FindNextFileW(find, &entry));
      FindClose(find);
    }
  }
  return ok ? kOk : kLaunchFailed;
}

int cmd_run(const std::vector<std::wstring>& args) {
  loader::LaunchOptions options;
  size_t i = 0;
  const auto value = [&](const wchar_t* option, std::wstring& out) {
    if (i + 1 >= args.size()) {
      std::fprintf(stderr, "option %s needs a value\n", narrow(option).c_str());
      return false;
    }
    out = args[++i];
    return true;
  };

  for (; i < args.size(); ++i) {
    const std::wstring& arg = args[i];
    std::wstring text;
    if (arg == L"--") {
      options.title_args.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
      break;
    } else if (arg == L"--exe") {
      if (!value(L"--exe", options.executable)) return kUsage;
    } else if (arg == L"--data-dir") {
      if (!value(L"--data-dir", options.data_dir)) return kUsage;
    } else if (arg == L"--no-sandbox") {
      options.sandbox = false;
    } else if (arg == L"--trace") {
      if (!value(L"--trace", options.trace_filter)) return kUsage;
    } else if (arg == L"--unimplemented") {
      if (!value(L"--unimplemented", text)) return kUsage;
      if (text == L"trap")
        options.unimplemented = loader::UnimplementedPolicy::Trap;
      else if (text == L"continue")
        options.unimplemented = loader::UnimplementedPolicy::Continue;
      else {
        std::fprintf(stderr, "--unimplemented expects trap or continue\n");
        return kUsage;
      }
    } else if (arg == L"--network") {
      if (!value(L"--network", text)) return kUsage;
      if (text == L"offline")
        options.offline = true;
      else if (text == L"host")
        options.offline = false;
      else {
        std::fprintf(stderr, "--network expects offline or host\n");
        return kUsage;
      }
    } else if (arg == L"--console") {
      if (!value(L"--console", text)) return kUsage;
      if (text == L"series-x")
        options.console_model = ConsoleModel::SeriesX;
      else if (text == L"series-s")
        options.console_model = ConsoleModel::SeriesS;
      else {
        std::fprintf(stderr, "--console expects series-x or series-s\n");
        return kUsage;
      }
    } else if (arg == L"--log-level") {
      if (!value(L"--log-level", text)) return kUsage;
      if (!log::parse_level(narrow(text), options.runtime_log_level)) {
        std::fprintf(stderr, "unknown log level: %s\n", narrow(text).c_str());
        return kUsage;
      }
    } else if (arg == L"--timeout") {
      if (!value(L"--timeout", text)) return kUsage;
      const unsigned long seconds = std::wcstoul(text.c_str(), nullptr, 10);
      if (seconds == 0 || seconds > 24 * 60 * 60) {
        std::fprintf(stderr, "--timeout expects a number of seconds\n");
        return kUsage;
      }
      options.timeout_ms = static_cast<uint32_t>(seconds * 1000);
    } else if (!arg.empty() && arg[0] == L'-') {
      std::fprintf(stderr, "unknown option: %s\n", narrow(arg).c_str());
      return kUsage;
    } else if (options.package_dir.empty()) {
      options.package_dir = arg;
    } else {
      std::fprintf(stderr, "unexpected argument: %s\n", narrow(arg).c_str());
      return kUsage;
    }
  }
  if (options.package_dir.empty()) {
    print_usage();
    return kUsage;
  }

  std::fflush(stdout);
  const loader::LaunchResult result = loader::launch(options);
  switch (result.status) {
    case loader::LaunchStatus::Failed:
      std::fprintf(stderr, "error: %s\n", result.error.c_str());
      return kLaunchFailed;
    case loader::LaunchStatus::TimedOut:
      std::fprintf(stderr, "holy: the title was stopped after the timeout\n");
      std::fprintf(stderr, "holy: runtime log: %s\n", narrow(result.log_path).c_str());
      return kTimedOut;
    case loader::LaunchStatus::Exited:
      break;
  }
  if (result.exit_code == 0) return kOk;
  std::fprintf(stderr, "holy: the title exited with code 0x%08X\n", result.exit_code);
  std::fprintf(stderr, "holy: runtime log: %s\n", narrow(result.log_path).c_str());
  return result.exit_code == kExitUnimplementedImport ? kUnimplemented : kTitleFailed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc < 2) {
    print_usage();
    return kUsage;
  }
  const std::wstring command = argv[1];
  const std::vector<std::wstring> args(argv + 2, argv + argc);
  if (command == L"inspect") return cmd_inspect(args);
  if (command == L"run") return cmd_run(args);
  if (command == L"version" || command == L"--version") {
    std::printf("holy %s\n", kVersion);
    return kOk;
  }
  if (command == L"help" || command == L"--help" || command == L"-h") {
    print_usage();
    return kOk;
  }
  std::fprintf(stderr, "unknown command: %s\n\n", narrow(command).c_str());
  print_usage();
  return kUsage;
}
