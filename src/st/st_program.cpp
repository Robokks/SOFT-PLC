#include "softplc/st/st_program.hpp"

#include <stdexcept>
#include <unordered_map>

#include "softplc/st/lexer.hpp"
#include "softplc/st/parser.hpp"

namespace softplc::st {

namespace {

using SymbolTable = std::unordered_map<std::string, tags::TagId>;

void bindExpr(Expr& expr, const SymbolTable& symbols) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return;
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(expr);
            auto it = symbols.find(id.name);
            if (it == symbols.end()) {
                throw std::runtime_error("undeclared identifier: " + id.name);
            }
            id.tagId = it->second;
            return;
        }
        case ExprKind::Unary:
            bindExpr(*static_cast<UnaryExpr&>(expr).operand, symbols);
            return;
        case ExprKind::Binary: {
            auto& binary = static_cast<BinaryExpr&>(expr);
            bindExpr(*binary.lhs, symbols);
            bindExpr(*binary.rhs, symbols);
            return;
        }
    }
}

void bindStmts(StmtList& stmts, const SymbolTable& symbols);

void bindStmt(Stmt& stmt, const SymbolTable& symbols) {
    switch (stmt.kind) {
        case StmtKind::Assign: {
            auto& assign = static_cast<AssignStmt&>(stmt);
            auto it = symbols.find(assign.target);
            if (it == symbols.end()) {
                throw std::runtime_error("undeclared assignment target: " + assign.target);
            }
            assign.targetId = it->second;
            bindExpr(*assign.value, symbols);
            return;
        }
        case StmtKind::If: {
            auto& ifStmt = static_cast<IfStmt&>(stmt);
            for (auto& branch : ifStmt.branches) {
                bindExpr(*branch.condition, symbols);
                bindStmts(branch.body, symbols);
            }
            bindStmts(ifStmt.elseBody, symbols);
            return;
        }
        case StmtKind::While: {
            auto& whileStmt = static_cast<WhileStmt&>(stmt);
            bindExpr(*whileStmt.condition, symbols);
            bindStmts(whileStmt.body, symbols);
            return;
        }
    }
}

void bindStmts(StmtList& stmts, const SymbolTable& symbols) {
    for (auto& stmt : stmts) {
        bindStmt(*stmt, symbols);
    }
}

}  // namespace

std::shared_ptr<StProgram> StProgram::load(std::string_view source, tags::TagStore& tags) {
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();

    Parser parser(std::move(tokens));
    StProgramAst ast = parser.parseProgram();

    SymbolTable symbols;
    for (const auto& decl : ast.varDecls) {
        tags::Value initial =
            decl.initialValue ? *decl.initialValue : tags::defaultValueFor(decl.type);
        const tags::TagId id = tags.declare(decl.name, decl.type, std::move(initial), decl.address);
        symbols.emplace(decl.name, id);
    }

    bindStmts(ast.body, symbols);

    return std::make_shared<StProgram>(std::move(ast));
}

StProgram::StProgram(StProgramAst ast) : ast_(std::move(ast)), interpreter_(ast_) {}

void StProgram::execute(core::ScanContext& ctx) { interpreter_.run(ctx.tags); }

std::string_view StProgram::name() const { return ast_.name; }

}  // namespace softplc::st
