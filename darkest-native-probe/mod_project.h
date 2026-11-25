#ifndef DARKEST_MOD_PROJECT_H
#define DARKEST_MOD_PROJECT_H

#include <cstdint>
#include <string>
#include <vector>

struct DDModProject {
    bool accepted = false;
    std::string game_data_path;
    std::string preview_icon;
    std::string short_description;
    std::string data_path;
    std::string title;
    std::string language;
    std::string update_details;
    std::string visibility;
    std::string upload_mode;
    std::string item_description;
    std::uint32_t version_major = 0;
    std::uint32_t version_minor = 0;
    std::uint32_t target_build = 0;
    std::uint32_t app_id = 0;
    std::uint64_t published_file_id = 0;
    std::vector<std::string> tags;
    std::vector<std::string> remove_tags;
};

// Reads metadata only. The caller owns resource resolution, activation and
// load order. The PC's ModDataPath value is not used as an iOS filesystem path.
DDModProject dd_read_mod_project(std::string xml);

#endif
