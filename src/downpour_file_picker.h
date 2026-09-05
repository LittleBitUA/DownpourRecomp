#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace downpour {

/**
 * Opens the platform file chooser for a user-owned install input.
 *
 * Example: PickFileWithNativeDialog("Select disc image", {"iso"});
 */
std::filesystem::path PickFileWithNativeDialog(
    std::string_view title, const std::vector<std::string_view>& extensions);

}  // namespace downpour
