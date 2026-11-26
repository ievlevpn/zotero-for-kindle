#ifndef FORMULA_PARSER_H_INCLUDED
#define FORMULA_PARSER_H_INCLUDED

// Minimal stub header replacing tinyxml2-based external XML symbol loading.

#include <map>
#include <string>

#include "atom/atom_basic.h"
#include "common.h"

namespace tex {

class TeXSymbolParser {
public:
  static const std::string RESOURCE_NAME;
  static const std::string DELIMITER_ATTR;
  static const std::string TYPE_ATTR;

  TeXSymbolParser() = delete;
  explicit TeXSymbolParser(const std::string& file);

  void readSymbols(std::map<std::string, sptr<SymbolAtom>>& res);
};

class TeXFormulaSettingParser {
public:
  static const std::string RESOURCE_NAME;

  TeXFormulaSettingParser() = delete;
  explicit TeXFormulaSettingParser(const std::string& file);

  void parseSymbol2Formula(
      std::map<int, std::string>& mappings,
      std::map<int, std::string>& textMappings);

  void parseSymbol(
      std::map<int, std::string>& mappings,
      std::map<int, std::string>& textMappings);
};

}  // namespace tex

#endif  // FORMULA_PARSER_H_INCLUDED
