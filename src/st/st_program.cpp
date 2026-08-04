#include "softplc/st/st_program.hpp"

#include <utility>

#include "softplc/st/lexer.hpp"
#include "softplc/st/parser.hpp"
#include "softplc/st/pou_binder.hpp"
#include "softplc/st/standard_fbs.hpp"

namespace softplc::st {

std::shared_ptr<StProgram> StProgram::load(std::string_view source, tags::TagStore& tags) {
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    CompilationUnit unit = parser.parseCompilationUnit();

    // Merge in the standard timer/counter FB library (TON/TOF/CTU/CTD) as ordinary
    // PouAsts -- unused unless the source actually instantiates one, so this costs
    // nothing beyond one extra parse pass over a small, fixed piece of text.
    Lexer stdLexer(kStandardFbLibrarySource);
    Parser stdParser(stdLexer.tokenize());
    std::vector<PouAst> standardPous = stdParser.parsePouLibrary();
    for (auto& pou : standardPous) {
        unit.pous.push_back(std::move(pou));
    }

    StProgramAst ast = bindCompilationUnit(std::move(unit), tags);

    return std::make_shared<StProgram>(std::move(ast));
}

StProgram::StProgram(StProgramAst ast) : ast_(std::move(ast)), interpreter_(ast_) {}

void StProgram::execute(core::ScanContext& ctx) { interpreter_.run(ctx.tags); }

std::string_view StProgram::name() const { return ast_.name; }

}  // namespace softplc::st
