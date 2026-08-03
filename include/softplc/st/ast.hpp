#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "softplc/tags/tag.hpp"
#include "softplc/tags/tag_store.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::st {

// ---- Expressions ----

enum class ExprKind { Literal, Identifier, Unary, Binary };
enum class UnaryOp { Neg, Not };
enum class BinaryOp { Add, Sub, Mul, Div, Eq, Ne, Lt, Gt, Le, Ge, And, Or, Xor };

class Expr {
public:
    explicit Expr(ExprKind kind) : kind(kind) {}
    virtual ~Expr() = default;

    const ExprKind kind;
};
using ExprPtr = std::unique_ptr<Expr>;

class LiteralExpr : public Expr {
public:
    explicit LiteralExpr(tags::Value value) : Expr(ExprKind::Literal), value(std::move(value)) {}
    tags::Value value;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(std::string name)
        : Expr(ExprKind::Identifier), name(std::move(name)) {}
    std::string name;
    // Resolved by StProgram::load() before the AST is ever executed.
    tags::TagId tagId = tags::kInvalidTagId;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(UnaryOp op, ExprPtr operand)
        : Expr(ExprKind::Unary), op(op), operand(std::move(operand)) {}
    UnaryOp op;
    ExprPtr operand;
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs)
        : Expr(ExprKind::Binary), op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

// ---- Statements ----

enum class StmtKind { Assign, If, While };

class Stmt {
public:
    explicit Stmt(StmtKind kind) : kind(kind) {}
    virtual ~Stmt() = default;

    const StmtKind kind;
};
using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

class AssignStmt : public Stmt {
public:
    AssignStmt(std::string target, ExprPtr value)
        : Stmt(StmtKind::Assign), target(std::move(target)), value(std::move(value)) {}
    std::string target;
    // Resolved by StProgram::load() before the AST is ever executed.
    tags::TagId targetId = tags::kInvalidTagId;
    ExprPtr value;
};

struct IfBranch {
    ExprPtr condition;
    StmtList body;
};

class IfStmt : public Stmt {
public:
    IfStmt() : Stmt(StmtKind::If) {}
    std::vector<IfBranch> branches;  // IF...THEN, followed by any ELSIF...THEN
    StmtList elseBody;               // empty if no ELSE
};

class WhileStmt : public Stmt {
public:
    WhileStmt(ExprPtr condition, StmtList body)
        : Stmt(StmtKind::While), condition(std::move(condition)), body(std::move(body)) {}
    ExprPtr condition;
    StmtList body;
};

// ---- Declarations & Program ----

enum class VarKind { Var, VarInput, VarOutput };

struct VarDecl {
    std::string name;
    tags::TypeId type = tags::TypeId::Bool;
    VarKind kind = VarKind::Var;
    std::optional<tags::Address> address;
    std::optional<tags::Value> initialValue;
};

struct StProgramAst {
    std::string name;
    std::vector<VarDecl> varDecls;
    StmtList body;
};

}  // namespace softplc::st
