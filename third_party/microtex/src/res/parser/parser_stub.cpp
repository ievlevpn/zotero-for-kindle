#include "res/parser/font_parser.h"
#include "res/parser/formula_parser.h"
#include "utils/exceptions.h"

namespace tex {

const std::string DefaultTeXFontParser::FONTS_RES_BASE = "fonts";
const std::string DefaultTeXFontParser::RESOURCE_NAME = "fonts/DefaultTeXFont.xml";
const std::string DefaultTeXFontParser::STYLE_MAPPING_EL = "TextStyleMapping";
const std::string DefaultTeXFontParser::GEN_SET_EL = "GeneralSettings";
const std::string DefaultTeXFontParser::MUFONTID_ATTR = "mufontid";
const std::string DefaultTeXFontParser::SPACEFONTID_ATTR = "spacefontid";

DefaultTeXFontParser::DefaultTeXFontParser() {
  throw ex_xml_parse("external XML font packs are not bundled");
}
DefaultTeXFontParser::DefaultTeXFontParser(const std::string& file) {
  throw ex_xml_parse(file + " not bundled");
}
DefaultTeXFontParser::DefaultTeXFontParser(const std::string&, const std::string& file) {
  throw ex_xml_parse(file + " not bundled");
}

void DefaultTeXFontParser::parseExtraPath() {}
void DefaultTeXFontParser::parseFontDescriptions(const std::string&) {}
void DefaultTeXFontParser::parseFontDescriptions() {}
void DefaultTeXFontParser::parseSymbolMappings(std::map<std::string, CharFont*>&) {}
std::string* DefaultTeXFontParser::parseDefaultTextStyleMappins() { return nullptr; }
void DefaultTeXFontParser::parseParameters(std::map<std::string, float>&) {}
void DefaultTeXFontParser::parseGeneralSettings(std::map<std::string, float>&) {}
std::map<std::string, std::vector<CharFont*>> DefaultTeXFontParser::parseTextStyleMappings() { return {}; }

const std::string TeXSymbolParser::RESOURCE_NAME = "TeXSymbols.xml";
const std::string TeXSymbolParser::DELIMITER_ATTR = "del";
const std::string TeXSymbolParser::TYPE_ATTR = "type";

TeXSymbolParser::TeXSymbolParser(const std::string& file) {
  throw ex_xml_parse(file + " not bundled");
}
void TeXSymbolParser::readSymbols(std::map<std::string, sptr<SymbolAtom>>&) {}

const std::string TeXFormulaSettingParser::RESOURCE_NAME = "TeXFormulaSettings.xml";

TeXFormulaSettingParser::TeXFormulaSettingParser(const std::string& file) {
  throw ex_xml_parse(file + " not bundled");
}
void TeXFormulaSettingParser::parseSymbol2Formula(std::map<int, std::string>&, std::map<int, std::string>&) {}
void TeXFormulaSettingParser::parseSymbol(std::map<int, std::string>&, std::map<int, std::string>&) {}

}  // namespace tex
