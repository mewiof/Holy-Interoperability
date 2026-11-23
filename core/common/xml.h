// Small, dependency-free XML reader for package manifests. It understands
// elements, attributes, character data, comments, processing instructions and
// the five predefined entities, which is all a manifest uses. Input is treated
// as untrusted: depth and size are bounded and malformed input is an error.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace holy::xml {

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<Node> children;
  std::string text;

  const std::string* attribute(std::string_view attribute_name) const;
  std::string attribute_or(std::string_view attribute_name, std::string_view fallback = {}) const;
  const Node* child(std::string_view child_name) const;
  std::vector<const Node*> children_named(std::string_view child_name) const;
};

// `document` is UTF-8 (with or without BOM) or UTF-16LE with BOM.
std::optional<Node> parse(std::string_view document, std::string* error = nullptr);

}  // namespace holy::xml
