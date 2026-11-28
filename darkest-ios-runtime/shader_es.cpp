#include "shader_es.h"

#include <locale>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <utility>

namespace darkest {
namespace {

constexpr char kPreamble[] = "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                              "precision highp sampler2D;\nprecision highp sampler3D;\n";

std::string RemoveComments(const std::string& source) {
  static const std::regex comment(R"(//[^\r\n]*|/\*[\s\S]*?\*/)");
  std::string clean = source;
  for (std::sregex_iterator it(source.begin(), source.end(), comment), end; it != end; ++it) {
    for (size_t i = it->position(); i < size_t(it->position() + it->length()); ++i)
      if (clean[i] != '\n' && clean[i] != '\r') clean[i] = ' ';
  }
  return clean;
}

std::vector<float> ParseFloatList(std::string_view values) {
  std::vector<float> result;
  for (;;) {
    const auto first = values.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
      throw std::invalid_argument("Missing shader uniform default");
    values.remove_prefix(first);
    const auto comma = values.find(',');
    auto part = values.substr(0, comma);
    part = part.substr(0, part.find_last_not_of(" \t\r\n") + 1);
    float number;
    std::istringstream value{std::string(part)};
    value.imbue(std::locale::classic());
    if (!(value >> number) || value.peek() != std::char_traits<char>::eof())
      throw std::invalid_argument("Unsupported shader uniform default");
    result.push_back(number);
    if (comma == std::string_view::npos) return result;
    values.remove_prefix(comma + 1);
  }
}

}  // namespace

MobileShader MakeMobileShader(std::string_view source, ShaderStage stage) {
  static const std::regex version(R"(^[ \t]*#[ \t]*version[ \t]+120[ \t]*\r?$)",
                                  std::regex_constants::multiline);
  static const std::regex line_placeholder(R"(^[ \t]*#[ \t]*line[ \t]+[.]+[ \t]*\r?$)",
                                           std::regex_constants::multiline);
  static const std::regex default_array(
      R"((\buniform\s+float\s+([A-Za-z_]\w*)\s*\[\s*(\d+)\s*\])\s*=\s*float\s*\[\s*\]\s*\(([^()]*)\)\s*;)");
  static const std::regex unhandled_default(R"(\buniform\b[^;]*=)");
  static const std::regex attribute(R"(\battribute\b)");
  static const std::regex varying(R"(\bvarying\b)");
  static const std::regex texture(R"(\btexture[23]D\b)");
  static const std::regex frag_color(R"(\bgl_FragColor\b)");
  static const std::regex gradient_offset(R"(\bYGradientStartLine\s*-\s*30(?![\w.]))");
  static const std::regex phase_multiplier(R"(\b3\.141569\s*\*\s*2(?![\w.]))");
  static const std::regex phase_bias(R"((\)\s*\+\s*1)(\s*\)\s*/\s*2\.0\b))");

  std::string clean = RemoveComments(std::string(source));
  if (!std::regex_search(clean, version)) throw std::invalid_argument("Expected GLSL 1.20");
  clean = std::regex_replace(clean, version, "");
  clean = std::regex_replace(clean, line_placeholder, "");

  MobileShader result;
  for (std::sregex_iterator it(clean.begin(), clean.end(), default_array), end; it != end; ++it) {
    auto values = ParseFloatList((*it)[4].str());
    if (values.size() != std::stoul((*it)[3].str()))
      throw std::invalid_argument("Shader uniform default count does not match array");
    result.defaults.push_back({(*it)[2].str(), std::move(values)});
  }
  clean = std::regex_replace(clean, default_array, "$1;");
  if (std::regex_search(clean, unhandled_default))
    throw std::invalid_argument("Unsupported initialized shader uniform");

  result.text = kPreamble;
  clean = std::regex_replace(clean, texture, "texture");
  if (stage == ShaderStage::Vertex) {
    clean = std::regex_replace(clean, attribute, "in");
    clean = std::regex_replace(clean, varying, "out");
    clean = std::regex_replace(clean, gradient_offset, "$&.0");
  } else {
    if (std::regex_search(clean, frag_color)) {
      result.text += "layout(location = 0) out vec4 dd_fragColor;\n";
      clean = std::regex_replace(clean, frag_color, "dd_fragColor");
    }
    clean = std::regex_replace(clean, varying, "in");
    clean = std::regex_replace(clean, phase_multiplier, "$&.0");
    clean = std::regex_replace(clean, phase_bias, "$1.0$2");
  }
  result.text += clean;
  return result;
}

}  // namespace darkest
