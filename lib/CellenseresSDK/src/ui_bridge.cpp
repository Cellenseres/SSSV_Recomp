#include "cs_sdk/ui_bridge.h"

#include <fstream>
#include <vector>

#define private public
#include "elements/ui_element.h"
#undef private

#include "RmlUi/Core.h"

namespace recompui {
    void queue_image_from_bytes_file(const std::string& src, const std::vector<char>& bytes);
}

namespace csdk::ui {
namespace {
bool read_file_bytes(const std::filesystem::path& path, std::vector<char>& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.seekg(0, std::ios::end);
    std::streamoff size = stream.tellg();
    if (size <= 0) {
        return false;
    }

    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    stream.read(out.data(), size);
    return stream.good();
}
}

bool queue_image_from_file(std::string_view src, const std::filesystem::path& path) {
    std::vector<char> bytes;
    if (!read_file_bytes(path, bytes)) {
        return false;
    }

    recompui::queue_image_from_bytes_file(std::string(src), bytes);
    return true;
}

bool prepend_child(recompui::Element* parent, recompui::Element* child) {
    if (parent == nullptr || child == nullptr || child->get_parent() != parent) {
        return false;
    }

    Rml::Element* parent_base = parent->base;
    Rml::Element* child_base = child->base;
    if (parent_base == nullptr || child_base == nullptr) {
        return false;
    }

    Rml::Element* first_child = parent_base->GetFirstChild();
    if (first_child == nullptr || first_child == child_base) {
        return true;
    }

    Rml::ElementPtr owned_child = parent_base->RemoveChild(child_base);
    if (!owned_child) {
        return false;
    }

    parent_base->InsertBefore(std::move(owned_child), first_child);
    return true;
}
} // namespace csdk::ui
