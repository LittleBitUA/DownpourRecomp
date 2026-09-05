#include "downpour_file_picker.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

namespace downpour {

std::filesystem::path PickFileWithNativeDialog(
    std::string_view title, const std::vector<std::string_view>& extensions) {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.title = [NSString stringWithUTF8String:std::string(title).c_str()];

    if (!extensions.empty()) {
      NSMutableArray<UTType*>* allowed_types = [NSMutableArray array];
      for (const std::string_view extension : extensions) {
        NSString* filename_extension =
            [NSString stringWithUTF8String:std::string(extension).c_str()];
        UTType* content_type = [UTType typeWithFilenameExtension:filename_extension];
        if (content_type) {
          [allowed_types addObject:content_type];
        }
      }
      panel.allowedContentTypes = allowed_types;
    }

    if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) {
      return {};
    }
    return std::filesystem::path(panel.URL.path.UTF8String);
  }
}

}  // namespace downpour
