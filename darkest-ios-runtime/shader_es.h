#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace darkest {

enum class ShaderStage { Vertex, Fragment };

struct UniformDefault {
  std::string name;
  std::vector<float> values;
};

struct MobileShader {
  std::string text;
  std::vector<UniformDefault> defaults;
};

// The caller applies defaults with glUniform1fv after a successful program
// link and before the game sets any uniforms on that program.
MobileShader MakeMobileShader(std::string_view source, ShaderStage stage);

}  // namespace darkest
