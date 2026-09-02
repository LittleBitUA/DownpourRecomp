#include "downpour_iso_installer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/overlay/acquire_wizard_overlay.h>

#include "downpour_title_update_installer.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <commdlg.h>
#include <windows.h>
#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

namespace downpour {

namespace {

// ----------------------------------------------------------------------------
// Minimal read-only XDVDFS (GDF) reader — the filesystem used by Xbox and
// Xbox 360 game discs. Directory tables are AVL trees of 4-byte-aligned
// entries; files are stored as contiguous sector runs, which keeps extraction
// a plain seek + copy per file. Layout reference: the freely documented
// XDVDFS volume format as implemented by extract-xiso and Xenia's GDFX code.
// ----------------------------------------------------------------------------

constexpr uint32_t kSectorSize = 2048;
constexpr std::string_view kVolumeMagic = "MICROSOFT*XBOX*MEDIA";
// The volume descriptor lives at sector 32 of the game partition. Redump-style
// Xbox 360 images place the game partition at 0xFD90000 (XGD2) or 0x2080000
// (XGD3); a bare partition dump has it at 0.
constexpr std::array<uint64_t, 3> kPartitionBases = {0x0ull, 0xFD90000ull, 0x2080000ull};
constexpr uint8_t kAttributeDirectory = 0x10;
constexpr uint16_t kEmptyDirectorySentinel = 0xFFFF;
constexpr uint32_t kMaxDirectoryDepth = 32;

uint16_t Le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct DiscFileEntry {
  std::filesystem::path relative_path;
  uint32_t start_sector = 0;
  uint32_t size = 0;
};

class XdvdfsImageReader {
 public:
  bool Open(const std::filesystem::path& iso_path, std::string& error) {
    file_.open(iso_path, std::ios::binary);
    if (!file_) {
      error = "Unable to open " + iso_path.string() + ".";
      return false;
    }
    std::array<char, kVolumeMagic.size()> magic{};
    for (const uint64_t base : kPartitionBases) {
      file_.clear();
      file_.seekg(static_cast<std::streamoff>(base + 32ull * kSectorSize));
      if (!file_.read(magic.data(), magic.size())) {
        continue;
      }
      if (std::string_view(magic.data(), magic.size()) == kVolumeMagic) {
        partition_base_ = base;
        return true;
      }
    }
    error =
        "The selected file is not an Xbox 360 disc image (no XDVDFS game "
        "partition found). Select a full-disc .iso dumped from your copy of "
        "Silent Hill: Downpour.";
    return false;
  }

  bool ListFiles(std::vector<DiscFileEntry>& files, std::string& error) {
    files.clear();
    std::array<uint8_t, 8> descriptor{};
    file_.clear();
    file_.seekg(
        static_cast<std::streamoff>(partition_base_ + 32ull * kSectorSize + kVolumeMagic.size()));
    if (!file_.read(reinterpret_cast<char*>(descriptor.data()), descriptor.size())) {
      error = "The disc image's volume descriptor is truncated.";
      return false;
    }
    const uint32_t root_sector = Le32(descriptor.data());
    const uint32_t root_size = Le32(descriptor.data() + 4);
    return WalkDirectory(root_sector, root_size, {}, 0, files, error);
  }

  bool HasXex2Magic(const DiscFileEntry& entry, std::string& error) {
    std::array<char, 4> magic{};
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(partition_base_ +
                                            static_cast<uint64_t>(entry.start_sector) *
                                                kSectorSize));
    if (!file_.read(magic.data(), magic.size())) {
      error = "The disc image's default.xex is truncated.";
      return false;
    }
    if (std::string_view(magic.data(), magic.size()) != "XEX2") {
      error = "The disc image's default.xex is corrupt (missing XEX2 header).";
      return false;
    }
    return true;
  }

  bool ExtractFile(const DiscFileEntry& entry, const std::filesystem::path& destination,
                   std::atomic<uint64_t>* copied_bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
      error = "Unable to create " + destination.parent_path().string() + ".";
      return false;
    }
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Unable to create " + destination.string() + ".";
      return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(partition_base_ +
                                            static_cast<uint64_t>(entry.start_sector) *
                                                kSectorSize));
    std::vector<char> buffer(4 * 1024 * 1024);
    uint64_t remaining = entry.size;
    while (remaining > 0) {
      const auto chunk =
          static_cast<std::streamsize>(std::min<uint64_t>(buffer.size(), remaining));
      if (!file_.read(buffer.data(), chunk)) {
        error = "Unexpected end of disc image while extracting " +
                entry.relative_path.string() + ".";
        return false;
      }
      out.write(buffer.data(), chunk);
      remaining -= static_cast<uint64_t>(chunk);
      if (copied_bytes) {
        copied_bytes->fetch_add(static_cast<uint64_t>(chunk), std::memory_order_relaxed);
      }
    }
    out.flush();
    if (!out) {
      error = "Failed to write " + destination.string() + ".";
      return false;
    }
    return true;
  }

 private:
  bool WalkDirectory(uint32_t table_sector, uint32_t table_size,
                     const std::filesystem::path& relative_dir,
                     uint32_t depth,
                     std::vector<DiscFileEntry>& files, std::string& error) {
    if (depth > kMaxDirectoryDepth) {
      error = "The disc image's directory tree exceeds the supported depth of " +
              std::to_string(kMaxDirectoryDepth) + ".";
      REXLOG_ERROR("{}", error);
      return false;
    }
    if (table_size == 0) {
      return true;  // Empty directory.
    }
    std::vector<uint8_t> table(table_size);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(partition_base_ +
                                            static_cast<uint64_t>(table_sector) * kSectorSize));
    if (!file_.read(reinterpret_cast<char*>(table.data()), table.size())) {
      error = "The disc image's directory table for '" + relative_dir.string() +
              "' is truncated.";
      return false;
    }
    // Iterative AVL walk; the visited set guards against malformed images with
    // cyclic child offsets.
    std::vector<uint32_t> pending{0};
    std::unordered_set<uint32_t> visited;
    while (!pending.empty()) {
      const uint32_t dword_offset = pending.back();
      pending.pop_back();
      if (!visited.insert(dword_offset).second) {
        continue;
      }
      const uint64_t offset = static_cast<uint64_t>(dword_offset) * 4;
      if (offset + 14 > table.size()) {
        continue;
      }
      const uint16_t left = Le16(table.data() + offset);
      const uint16_t right = Le16(table.data() + offset + 2);
      if (left == kEmptyDirectorySentinel) {
        continue;
      }
      const uint32_t start_sector = Le32(table.data() + offset + 4);
      const uint32_t size = Le32(table.data() + offset + 8);
      const uint8_t attributes = table[offset + 12];
      const uint8_t name_length = table[offset + 13];
      if (offset + 14 + name_length <= table.size()) {
        const std::string name(reinterpret_cast<const char*>(table.data() + offset + 14),
                               name_length);
        const std::filesystem::path name_path(name);
        if (name.empty() || name == "." || name == ".." ||
            name.find('\0') != std::string::npos || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name_path.is_absolute() ||
            name_path.has_root_name() || name_path.has_root_directory()) {
          error = "The disc image contains an unsafe directory entry name.";
          REXLOG_ERROR("Rejecting unsafe XDVDFS entry name '{}' under '{}'.", name,
                       relative_dir.string());
          return false;
        }
        const auto child_path = relative_dir / name_path;
        if (attributes & kAttributeDirectory) {
          if (!WalkDirectory(start_sector, size, child_path, depth + 1, files,
                             error)) {
            return false;
          }
        } else {
          files.push_back({child_path, start_sector, size});
        }
      }
      if (left != 0 && left != kEmptyDirectorySentinel) {
        pending.push_back(left);
      }
      if (right != 0 && right != kEmptyDirectorySentinel) {
        pending.push_back(right);
      }
    }
    return true;
  }

  std::ifstream file_;
  uint64_t partition_base_ = 0;
};

// ----------------------------------------------------------------------------
// File picker
// ----------------------------------------------------------------------------

#if defined(_WIN32)
std::filesystem::path PickIsoFile() {
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select your Silent Hill: Downpour Xbox 360 disc image";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) {
    return {};
  }
  return filename;
}
#elif defined(__APPLE__)
std::filesystem::path PickIsoFile() {
  REXLOG_ERROR("The ISO file picker is not implemented on macOS.");
  return {};
}
#else
std::filesystem::path PickIsoFile() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Select your Silent Hill: Downpour Xbox 360 disc image", nullptr,
      GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
      nullptr);
  if (!dialog) {
    return {};
  }

  GtkFileFilter* iso_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(iso_filter, "Xbox 360 disc image (*.iso)");
  gtk_file_filter_add_pattern(iso_filter, "*.iso");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), iso_filter);
  GtkFileFilter* all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All files");
  gtk_file_filter_add_pattern(all_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

  std::filesystem::path result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename) {
      result = filename;
      g_free(filename);
    }
  }

  gtk_widget_destroy(dialog);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return result;
}
#endif

}  // namespace

bool IsGameDataInstalled(const std::filesystem::path& game_root) {
  std::error_code ec;
  return std::filesystem::is_regular_file(game_root / "default.xex", ec) && !ec;
}

bool InstallGameDataFromIso(const std::filesystem::path& iso_path,
                            const std::filesystem::path& game_root,
                            std::atomic<uint64_t>* copied_bytes,
                            std::atomic<uint64_t>* total_bytes, std::string& error) {
  XdvdfsImageReader reader;
  if (!reader.Open(iso_path, error)) {
    return false;
  }
  std::vector<DiscFileEntry> files;
  if (!reader.ListFiles(files, error)) {
    return false;
  }
  const auto xex = std::find_if(files.begin(), files.end(), [](const DiscFileEntry& f) {
    return f.relative_path == "default.xex";
  });
  if (xex == files.end()) {
    error =
        "The disc image does not contain default.xex at its root; it is not a "
        "Silent Hill: Downpour game disc.";
    return false;
  }
  if (!reader.HasXex2Magic(*xex, error)) {
    return false;
  }

  uint64_t total = 0;
  for (const auto& f : files) {
    total += f.size;
  }
  if (total_bytes) {
    total_bytes->store(total, std::memory_order_relaxed);
  }
  std::error_code ec;
  const auto space = std::filesystem::space(
      std::filesystem::exists(game_root, ec) ? game_root : game_root.parent_path(), ec);
  if (!ec && space.available < total + (512ull << 20)) {
    error = "Not enough free disk space to extract the game data (need ~" +
            std::to_string((total >> 30) + 1) + " GiB free).";
    return false;
  }

  // Extract in disc order for sequential reads, except default.xex, which
  // goes last: it doubles as the "install complete" marker
  // (IsGameDataInstalled), so writing it only after everything else
  // guarantees an interrupted extraction re-opens the installer on next
  // launch and resumes. Files already extracted with the right size are
  // skipped, so that resume is cheap.
  std::sort(files.begin(), files.end(), [](const DiscFileEntry& a, const DiscFileEntry& b) {
    const bool a_is_marker = a.relative_path == "default.xex";
    const bool b_is_marker = b.relative_path == "default.xex";
    if (a_is_marker != b_is_marker) {
      return b_is_marker;
    }
    return a.start_sector < b.start_sector;
  });
  for (const auto& f : files) {
    const auto destination = game_root / f.relative_path;
    std::error_code exists_ec;
    if (std::filesystem::is_regular_file(destination, exists_ec) &&
        std::filesystem::file_size(destination, exists_ec) == f.size && !exists_ec) {
      if (copied_bytes) {
        copied_bytes->fetch_add(f.size, std::memory_order_relaxed);
      }
      continue;
    }
    if (!reader.ExtractFile(f, destination, copied_bytes, error)) {
      return false;
    }
  }
  REXLOG_INFO("Extracted {} files ({} MiB) from {} into {}", files.size(), total >> 20,
              iso_path.string(), game_root.string());
  return true;
}

void ShowIsoInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                          std::function<void(rex::PathConfig)> complete) {
  const auto game_root = runtime_paths.game_data_root;

  rex::ui::AcquireWizardDialog::Options options;
  // ASCII only: the overlay font atlas has no em-dash glyph (renders '?').
  options.title = "Silent Hill: Downpour - Game Data";
  options.intro =
      "This port needs the game files from your own legally-owned Xbox 360 copy "
      "of Silent Hill: Downpour. Select your disc image (.iso) and its contents "
      "will be extracted here - nothing else to do.";
  options.target_directory = game_root.string();
  options.initial_status =
      "Select the disc image dumped from your copy of the game (USA or Europe "
      "release). Extraction needs ~4.5 GiB of free space.";
  // No fetch button: the game data cannot be downloaded, only extracted from
  // the user's own dump.
  options.pick_button_label = "Select disc image...";
  options.install_working_status = "Extracting game data... (a few minutes)";
  options.done_status = "Game data installed.";
  options.done_button_label = "Continue";

  auto install = [game_root](const std::filesystem::path& source,
                             std::atomic<uint64_t>& copied_bytes,
                             std::atomic<uint64_t>& total_bytes, std::string& error) {
    if (!InstallGameDataFromIso(source, game_root, &copied_bytes, &total_bytes, error)) {
      return false;
    }
    if (!IsGameDataInstalled(game_root)) {
      error = "The game data could not be verified after extraction.";
      return false;
    }
    // Best-effort: stage Title Update 1 right away so the freshly-installed
    // game boots without a second wizard. On failure (offline, mirror down)
    // the existing TU wizard still appears on the next boot as the fallback.
    std::string tu_error;
    if (!IsTitleUpdateInstalled(game_root) &&
        !TryDownloadAndStageTitleUpdate(game_root, tu_error)) {
      REXLOG_WARN("Title update auto-staging after ISO install failed: {}", tu_error);
    }
    return true;
  };

  new rex::ui::AcquireWizardDialog(
      drawer, std::move(options), /*fetch=*/nullptr, []() { return PickIsoFile(); },
      std::move(install),
      [runtime_paths = std::move(runtime_paths), complete = std::move(complete)]() mutable {
        // Same rationale as the title update wizard: resuming the runtime
        // inline hangs on Win32, so restart the process; the fresh launch
        // sees the game data (and usually TU1) already installed.
        RelaunchSelfOrResume(std::move(runtime_paths), std::move(complete));
      });
}

}  // namespace downpour
