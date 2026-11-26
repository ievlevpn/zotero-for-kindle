#ifndef FONT_PARSER_H_INCLUDED
#define FONT_PARSER_H_INCLUDED

// Minimal stub header replacing tinyxml2-based external XML alphabet loading.
// All built-in math fonts, symbols, and metrics are compiled into src/res/{font,sym,builtin}/.

#include <map>
#include <string>
#include <vector>

#include "fonts/font_basic.h"
#include "fonts/font_info.h"

namespace tex {

class DefaultTeXFontParser {
public:
  static const std::string FONTS_RES_BASE;
  static const std::string RESOURCE_NAME;
  static const std::string STYLE_MAPPING_EL;
  static const std::string GEN_SET_EL;
  static const std::string MUFONTID_ATTR;
  static const std::string SPACEFONTID_ATTR;

  DefaultTeXFontParser();
  explicit DefaultTeXFontParser(const std::string& file);
  DefaultTeXFontParser(const std::string& base, const std::string& file);

  void parseExtraPath();
  void parseFontDescriptions(const std::string& file);
  void parseFontDescriptions();
  void parseSymbolMappings(std::map<std::string, CharFont*>& res);
  std::string* parseDefaultTextStyleMappins();
  void parseParameters(std::map<std::string, float>& res);
  void parseGeneralSettings(std::map<std::string, float>& res);
  std::map<std::string, std::vector<CharFont*>> parseTextStyleMappings();
};

}  // namespace tex

#endif  // FONT_PARSER_H_INCLUDED
