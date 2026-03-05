#pragma once

#include <filesystem>
#include <string_view>

namespace recompui {
    class Element;
}

namespace csdk::ui {
    bool queue_image_from_file(std::string_view src, const std::filesystem::path& path);
    bool prepend_child(recompui::Element* parent, recompui::Element* child);
}
