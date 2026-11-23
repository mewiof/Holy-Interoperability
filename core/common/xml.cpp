#include "core/common/xml.h"

#include "core/common/strings.h"

namespace holy::xml {
namespace {

constexpr int kMaxDepth = 64;
constexpr size_t kMaxNodes = 100000;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Node> run(std::string* error) {
    Node root;
    if (!skip_misc() || !parse_element(root, 0)) {
      if (error) *error = error_.empty() ? "malformed XML" : error_;
      return std::nullopt;
    }
    return root;
  }

 private:
  bool fail(const char* message) {
    if (error_.empty()) error_ = std::string(message) + " at offset " + std::to_string(pos_);
    return false;
  }

  bool at_end() const { return pos_ >= text_.size(); }
  bool looking_at(std::string_view token) const { return text_.substr(pos_, token.size()) == token; }

  void skip_space() {
    while (!at_end() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) {
      ++pos_;
    }
  }

  bool skip_until(std::string_view terminator) {
    const size_t found = text_.find(terminator, pos_);
    if (found == std::string_view::npos) return fail("unterminated construct");
    pos_ = found + terminator.size();
    return true;
  }

  // Skips whitespace, comments, processing instructions and DOCTYPE.
  bool skip_misc() {
    for (;;) {
      skip_space();
      if (looking_at("<?")) {
        if (!skip_until("?>")) return false;
      } else if (looking_at("<!--")) {
        if (!skip_until("-->")) return false;
      } else if (looking_at("<!")) {
        if (!skip_until(">")) return false;
      } else {
        return true;
      }
    }
  }

  static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == ':' || (static_cast<unsigned char>(c) >= 0x80);
  }

  bool parse_name(std::string& out) {
    const size_t start = pos_;
    while (!at_end() && is_name_char(text_[pos_])) ++pos_;
    if (pos_ == start) return fail("expected a name");
    out.assign(text_.substr(start, pos_ - start));
    return true;
  }

  static void append_decoded(std::string& out, std::string_view raw) {
    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] != '&') {
        out.push_back(raw[i++]);
        continue;
      }
      const size_t semicolon = raw.find(';', i);
      const std::string_view entity =
          semicolon == std::string_view::npos ? std::string_view{} : raw.substr(i + 1, semicolon - i - 1);
      if (entity == "amp")
        out.push_back('&');
      else if (entity == "lt")
        out.push_back('<');
      else if (entity == "gt")
        out.push_back('>');
      else if (entity == "quot")
        out.push_back('"');
      else if (entity == "apos")
        out.push_back('\'');
      else {
        out.push_back(raw[i++]);
        continue;
      }
      i = semicolon + 1;
    }
  }

  bool parse_attribute(Node& node) {
    std::string name;
    if (!parse_name(name)) return false;
    skip_space();
    if (at_end() || text_[pos_] != '=') return fail("expected '=' after attribute name");
    ++pos_;
    skip_space();
    if (at_end() || (text_[pos_] != '"' && text_[pos_] != '\''))
      return fail("expected a quoted attribute value");
    const char quote = text_[pos_++];
    const size_t end = text_.find(quote, pos_);
    if (end == std::string_view::npos) return fail("unterminated attribute value");
    std::string value;
    append_decoded(value, text_.substr(pos_, end - pos_));
    pos_ = end + 1;
    node.attributes.emplace_back(std::move(name), std::move(value));
    return true;
  }

  bool parse_element(Node& node, int depth) {
    if (depth > kMaxDepth) return fail("document is nested too deeply");
    if (++node_count_ > kMaxNodes) return fail("document has too many elements");
    if (at_end() || text_[pos_] != '<') return fail("expected an element");
    ++pos_;
    if (!parse_name(node.name)) return false;

    for (;;) {
      skip_space();
      if (at_end()) return fail("unterminated start tag");
      if (looking_at("/>")) {
        pos_ += 2;
        return true;
      }
      if (text_[pos_] == '>') {
        ++pos_;
        break;
      }
      if (!parse_attribute(node)) return false;
    }

    for (;;) {
      const size_t open = text_.find('<', pos_);
      if (open == std::string_view::npos) return fail("unterminated element");
      append_decoded(node.text, text_.substr(pos_, open - pos_));
      pos_ = open;

      if (looking_at("</")) {
        pos_ += 2;
        std::string closing;
        if (!parse_name(closing)) return false;
        if (closing != node.name) return fail("mismatched closing tag");
        skip_space();
        if (at_end() || text_[pos_] != '>') return fail("malformed closing tag");
        ++pos_;
        return true;
      }
      if (looking_at("<!--")) {
        if (!skip_until("-->")) return false;
      } else if (looking_at("<![CDATA[")) {
        const size_t start = pos_ + 9;
        const size_t end = text_.find("]]>", start);
        if (end == std::string_view::npos) return fail("unterminated CDATA section");
        node.text.append(text_.substr(start, end - start));
        pos_ = end + 3;
      } else if (looking_at("<?")) {
        if (!skip_until("?>")) return false;
      } else {
        Node child;
        if (!parse_element(child, depth + 1)) return false;
        node.children.push_back(std::move(child));
      }
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
  size_t node_count_ = 0;
  std::string error_;
};

}  // namespace

const std::string* Node::attribute(std::string_view attribute_name) const {
  for (const auto& [key, value] : attributes) {
    if (key == attribute_name) return &value;
  }
  return nullptr;
}

std::string Node::attribute_or(std::string_view attribute_name, std::string_view fallback) const {
  const std::string* value = attribute(attribute_name);
  return value ? *value : std::string(fallback);
}

const Node* Node::child(std::string_view child_name) const {
  for (const Node& node : children) {
    if (node.name == child_name) return &node;
  }
  return nullptr;
}

std::vector<const Node*> Node::children_named(std::string_view child_name) const {
  std::vector<const Node*> out;
  for (const Node& node : children) {
    if (node.name == child_name) out.push_back(&node);
  }
  return out;
}

std::optional<Node> parse(std::string_view document, std::string* error) {
  std::string converted;
  if (document.size() >= 2 && static_cast<unsigned char>(document[0]) == 0xFF &&
      static_cast<unsigned char>(document[1]) == 0xFE) {
    const std::wstring_view wide(reinterpret_cast<const wchar_t*>(document.data() + 2),
                                 (document.size() - 2) / 2);
    converted = narrow(wide);
    document = converted;
  } else if (document.size() >= 3 && static_cast<unsigned char>(document[0]) == 0xEF &&
             static_cast<unsigned char>(document[1]) == 0xBB &&
             static_cast<unsigned char>(document[2]) == 0xBF) {
    document.remove_prefix(3);
  }
  return Parser(document).run(error);
}

}  // namespace holy::xml
