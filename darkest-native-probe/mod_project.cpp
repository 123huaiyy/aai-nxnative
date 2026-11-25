#include "mod_project.h"
#include "vendor/rapidxml/rapidxml.hpp"

#include <cstdlib>
#include <string_view>

namespace {
using Node = rapidxml::xml_node<char>;

std::string field(const Node *node, std::size_t capacity = 512)
{
    return node ? std::string(std::string_view(node->value()).substr(0, capacity - 1))
                : std::string{};
}

long long number(const Node *node)
{
    return node ? std::strtoll(node->value(), nullptr, 10) : 0;
}

std::vector<std::string> tags(const Node *parent)
{
    std::vector<std::string> values;
    if (parent) {
        for (auto child = parent->first_node(); child; child = child->next_sibling()) {
            values.push_back(field(child));
        }
    }
    return values;
}
}

DDModProject dd_read_mod_project(std::string xml)
{
    rapidxml::xml_document<char> document;
    try {
        document.parse<0>(xml.data());
    } catch (const rapidxml::parse_error&) {
        return {};
    }

    DDModProject result;
    const auto *project = document.first_node("project");
    if (!project) {
        return result;
    }

    const auto *data_path = project->first_node("ModDataPath");
    const auto *title = project->first_node("Title");
    result.accepted = data_path && title;
    result.game_data_path = field(project->first_node("GameDataPath"));
    result.preview_icon = field(project->first_node("PreviewIconFile"));
    result.short_description = field(project->first_node("ItemDescriptionShort"));
    result.data_path = field(data_path);
    result.title = field(title);
    result.language = field(project->first_node("Language"));
    result.update_details = field(project->first_node("UpdateDetails"), 8000);
    result.visibility = field(project->first_node("Visibility"));
    result.upload_mode = field(project->first_node("UploadMode"));
    result.item_description = field(project->first_node("ItemDescription"), 8000);
    result.version_major = static_cast<std::uint32_t>(number(project->first_node("VersionMajor")));
    result.version_minor = static_cast<std::uint32_t>(number(project->first_node("VersionMinor")));
    result.target_build = static_cast<std::uint32_t>(number(project->first_node("TargetBuild")));
    result.app_id = static_cast<std::uint32_t>(number(project->first_node("AppId")));
    result.published_file_id = static_cast<std::uint64_t>(number(project->first_node("PublishedFileId")));
    result.tags = tags(project->first_node("Tags"));
    result.remove_tags = tags(project->first_node("RemoveTags"));
    return result;
}
