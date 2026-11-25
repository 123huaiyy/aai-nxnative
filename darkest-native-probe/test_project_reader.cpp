#include "mod_project.h"

#include <fstream>
#include <iostream>
#include <iterator>

void from_bytes(const char *key, const std::string &data)
{
    constexpr char digits[] = "0123456789abcdef";
    std::cout << key << '\t';
    for (unsigned char byte : data) {
        std::cout << digits[byte >> 4] << digits[byte & 15];
    }
    std::cout << '\n';
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        return 1;
    }
    const DDModProject project = dd_read_mod_project(
        std::string(std::istreambuf_iterator<char>(input), {}));
    std::cout << "ok\t" << project.accepted << '\n'
              << "version_major\t" << project.version_major << '\n'
              << "version_minor\t" << project.version_minor << '\n'
              << "target_build\t" << project.target_build << '\n'
              << "app_id\t" << project.app_id << '\n'
              << "published_file_id\t" << project.published_file_id << '\n';
    from_bytes("game_data_path", project.game_data_path);
    from_bytes("preview_icon", project.preview_icon);
    from_bytes("short_description", project.short_description);
    from_bytes("data_path", project.data_path);
    from_bytes("title", project.title);
    from_bytes("language", project.language);
    from_bytes("update_details", project.update_details);
    from_bytes("visibility", project.visibility);
    from_bytes("upload_mode", project.upload_mode);
    from_bytes("item_description", project.item_description);
    for (const auto &tag : project.tags) {
        from_bytes("tag", tag);
    }
    for (const auto &tag : project.remove_tags) {
        from_bytes("remove_tag", tag);
    }
}
