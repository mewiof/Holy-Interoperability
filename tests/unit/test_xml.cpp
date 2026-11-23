#include <string>

#include "core/common/xml.h"
#include "tests/unit/test.h"

using namespace holy;

TEST(xml_parses_elements_attributes_and_text) {
  const auto root = xml::parse(
      "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- comment -->\n"
      "<Root a=\"1\" b='two &amp; three'><Child>text &lt;here&gt;</Child><Empty/><Child x=\"y\"/></Root>");
  EXPECT(root.has_value());
  if (!root) return;
  EXPECT(root->name == "Root");
  EXPECT(root->attribute_or("a") == "1");
  EXPECT(root->attribute_or("b") == "two & three");
  EXPECT(root->attribute("missing") == nullptr);
  EXPECT(root->children.size() == 3);
  EXPECT(root->child("Child") != nullptr && root->child("Child")->text == "text <here>");
  EXPECT(root->children_named("Child").size() == 2);
  EXPECT(root->child("Empty") != nullptr);
}

TEST(xml_rejects_malformed_documents) {
  std::string error;
  EXPECT(!xml::parse("<a><b></a>", &error).has_value());
  EXPECT(!error.empty());
  EXPECT(!xml::parse("<a attr=novalue/>").has_value());
  EXPECT(!xml::parse("<a>").has_value());
  EXPECT(!xml::parse("").has_value());
  EXPECT(!xml::parse("plain text").has_value());
}

TEST(xml_bounds_nesting_depth) {
  std::string deep;
  for (int i = 0; i < 1000; ++i) deep += "<a>";
  for (int i = 0; i < 1000; ++i) deep += "</a>";
  EXPECT(!xml::parse(deep).has_value());
}

TEST(xml_reads_utf16_with_bom) {
  const wchar_t wide[] = L"\xFEFF<Root name=\"v\"/>";
  const std::string_view bytes(reinterpret_cast<const char*>(wide), sizeof(wide) - sizeof(wchar_t));
  const auto root = xml::parse(bytes);
  EXPECT(root.has_value());
  if (root) EXPECT(root->attribute_or("name") == "v");
}
