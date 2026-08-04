#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

    // Deep-copies this node (and its subtree). Any resolved TagId is reset to
    // tags::kInvalidTagId -- a clone is always re-bound by the caller (see
    // pou_binder.hpp), never assumed to already be bound.
    [[nodiscard]] virtual std::unique_ptr<Expr> clone() const = 0;

    const ExprKind kind;
};
using ExprPtr = std::unique_ptr<Expr>;

class LiteralExpr : public Expr {
public:
    explicit LiteralExpr(tags::Value value) : Expr(ExprKind::Literal), value(std::move(value)) {}
    [[nodiscard]] ExprPtr clone() const override { return std::make_unique<LiteralExpr>(value); }
    tags::Value value;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(std::string name)
        : Expr(ExprKind::Identifier), name(std::move(name)) {}
    [[nodiscard]] ExprPtr clone() const override { return std::make_unique<IdentifierExpr>(name); }
    std::string name;
    // Resolved once, at bind time (by StProgram::load() or pou_binder). A dotted
    // name (e.g. "DB1.Speed" or, within a POU body, a bare member name later
    // prefixed at bind time) resolves the same way as any other tag name.
    tags::TagId tagId = tags::kInvalidTagId;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(UnaryOp op, ExprPtr operand)
        : Expr(ExprKind::Unary), op(op), operand(std::move(operand)) {}
    [[nodiscard]] ExprPtr clone() const override {
        return std::make_unique<UnaryExpr>(op, operand->clone());
    }
    UnaryOp op;
    ExprPtr operand;
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs)
        : Expr(ExprKind::Binary), op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
    [[nodiscard]] ExprPtr clone() const override {
        return std::make_unique<BinaryExpr>(op, lhs->clone(), rhs->clone());
    }
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

// ---- Statements ----

enum class StmtKind { Assign, If, While, Call, Rung };

class Stmt {
public:
    explicit Stmt(StmtKind kind) : kind(kind) {}
    virtual ~Stmt() = default;

    [[nodiscard]] virtual std::unique_ptr<Stmt> clone() const = 0;

    const StmtKind kind;
};
using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

// Deep-copies every statement in `stmts` into a new StmtList. Shared by every Stmt
// subtype that owns a nested body (IfStmt branches/elseBody, WhileStmt body) and by
// pou_binder when cloning a whole POU body for a fresh instance/call site. Only
// needs Stmt's virtual clone() (dispatched polymorphically), so it's defined here
// rather than needing a separate .cpp.
inline StmtList cloneStmtList(const StmtList& stmts) {
    StmtList copy;
    copy.reserve(stmts.size());
    for (const auto& stmt : stmts) {
        copy.push_back(stmt->clone());
    }
    return copy;
}

class AssignStmt : public Stmt {
public:
    AssignStmt(std::string target, ExprPtr value)
        : Stmt(StmtKind::Assign), target(std::move(target)), value(std::move(value)) {}
    [[nodiscard]] StmtPtr clone() const override {
        return std::make_unique<AssignStmt>(target, value->clone());
    }
    std::string target;
    // Resolved once, at bind time.
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
    [[nodiscard]] StmtPtr clone() const override {
        auto copy = std::make_unique<IfStmt>();
        copy->branches.reserve(branches.size());
        for (const auto& branch : branches) {
            copy->branches.push_back(IfBranch{branch.condition->clone(), cloneStmtList(branch.body)});
        }
        copy->elseBody = cloneStmtList(elseBody);
        return copy;
    }
    std::vector<IfBranch> branches;  // IF...THEN, followed by any ELSIF...THEN
    StmtList elseBody;               // empty if no ELSE
};

class WhileStmt : public Stmt {
public:
    WhileStmt(ExprPtr condition, StmtList body)
        : Stmt(StmtKind::While), condition(std::move(condition)), body(std::move(body)) {}
    [[nodiscard]] StmtPtr clone() const override {
        return std::make_unique<WhileStmt>(condition->clone(), cloneStmtList(body));
    }
    ExprPtr condition;
    StmtList body;
};

// One argument of a CallStmt: either an input binding (`Param := expr`, evaluated in
// the CALLER's scope) or an output binding (`Param => target`, a possibly-dotted
// lvalue name in the caller's scope that the callee's output is copied into after
// the call). Exactly one of inputExpr/outputTargetName is set, matching isOutput.
struct CallArg {
    std::string paramName;
    bool isOutput = false;
    ExprPtr inputExpr;               // set when !isOutput
    std::string outputTargetName;    // set when isOutput

    // Resolved once, at bind time: paramTagId is the CALLEE's tag for paramName;
    // outputTargetId (isOutput only) is the CALLER's tag for outputTargetName.
    tags::TagId paramTagId = tags::kInvalidTagId;
    tags::TagId outputTargetId = tags::kInvalidTagId;

    [[nodiscard]] CallArg clone() const {
        CallArg copy;
        copy.paramName = paramName;
        copy.isOutput = isOutput;
        copy.outputTargetName = outputTargetName;
        if (inputExpr) copy.inputExpr = inputExpr->clone();
        return copy;
    }
};

inline constexpr std::size_t kInvalidFrame = static_cast<std::size_t>(-1);

// A call to an FB instance or an FC. Statement-only in v1 (no expression-position
// calls) -- deliberately, so the same shape ("a box with named in/out pins") is
// reusable by a future Ladder Diagram front-end for FB instances placed on a rung.
class CallStmt : public Stmt {
public:
    CallStmt(std::string calleeName, std::vector<CallArg> args)
        : Stmt(StmtKind::Call), calleeName(std::move(calleeName)), args(std::move(args)) {}
    [[nodiscard]] StmtPtr clone() const override {
        std::vector<CallArg> copiedArgs;
        copiedArgs.reserve(args.size());
        for (const auto& arg : args) copiedArgs.push_back(arg.clone());
        return std::make_unique<CallStmt>(calleeName, std::move(copiedArgs));
    }
    std::string calleeName;
    std::vector<CallArg> args;
    // Resolved once, at bind time: index into the owning StProgramAst::frames for
    // the bound (cloned) body this call executes.
    std::size_t frameIndex = kInvalidFrame;
};

// A ladder-style rung: `RUNG <bool-expr> => [SET|RESET] target (',' [SET|RESET] target)* ;`.
// The condition expression reuses the ordinary boolean expression grammar --
// AND/OR/NOT already give exactly "series contacts / parallel branches / normally-
// closed contact" semantics, so no separate contact/branch AST was needed. An FB/FC
// box placed "on" a rung is just an ordinary CallStmt in the same body, adjacent to
// the RUNG that reads its output or drives its input (see docs/architecture.md).
enum class RungOutputKind { Direct, Set, Reset };

struct RungOutput {
    RungOutputKind kind = RungOutputKind::Direct;
    std::string targetName;
    // Resolved once, at bind time. The target must be a BOOL tag (a coil).
    tags::TagId targetId = tags::kInvalidTagId;

    [[nodiscard]] RungOutput clone() const {
        return RungOutput{.kind = kind, .targetName = targetName};
    }
};

class RungStmt : public Stmt {
public:
    RungStmt(ExprPtr condition, std::vector<RungOutput> outputs)
        : Stmt(StmtKind::Rung), condition(std::move(condition)), outputs(std::move(outputs)) {}
    [[nodiscard]] StmtPtr clone() const override {
        std::vector<RungOutput> copiedOutputs;
        copiedOutputs.reserve(outputs.size());
        for (const auto& out : outputs) copiedOutputs.push_back(out.clone());
        return std::make_unique<RungStmt>(condition->clone(), std::move(copiedOutputs));
    }
    ExprPtr condition;
    std::vector<RungOutput> outputs;
};

// ---- Declarations & Program ----

enum class VarKind { Var, VarInput, VarOutput, VarTemp };

struct VarDecl {
    std::string name;
    VarKind kind = VarKind::Var;
    // Exactly one of elementaryType/instanceOfType is set. elementaryType: an
    // ordinary BOOL/INT/.../STRING variable. instanceOfType: the name of a
    // FUNCTION_BLOCK this declares an instance of (e.g. "FB_MotorControl") --
    // restricted to VarKind::Var, and only inside PROGRAM/FUNCTION_BLOCK bodies.
    std::optional<tags::TypeId> elementaryType;
    std::optional<std::string> instanceOfType;
    std::optional<tags::Address> address;      // elementary vars only
    std::optional<tags::Value> initialValue;   // elementary vars only
};

// One bound (cloned + tag-resolved) executable body: one per FB instance
// declaration (shared by every CallStmt invoking that instance) or one per FC call
// site (not shared across call sites, so each site's VAR_TEMP/VAR_OUTPUT stays
// independently inspectable). Owned centrally by StProgramAst::frames and referenced
// by plain index (CallStmt::frameIndex), the same "index, not a pointer" idiom as
// tags::TagId.
struct Frame {
    StmtList body;
    // VAR_TEMP members only: reset to this value before every execution of this
    // frame (FC call-site scratch space must not retain state between calls).
    std::vector<std::pair<tags::TagId, tags::Value>> tempResets;
};

// A FUNCTION_BLOCK or FUNCTION definition, as parsed -- not yet bound to any tags.
// isFunction distinguishes a stateless FUNCTION (no VAR section, every VAR_INPUT
// must be explicitly bound at each call) from a FUNCTION_BLOCK (has persistent VAR,
// unbound VAR_INPUTs keep their last value).
struct PouAst {
    std::string name;
    bool isFunction = false;
    std::vector<VarDecl> varDecls;
    StmtList body;
};

// A DATA_BLOCK definition: a named, persistent record with no code. Members are
// declared into TagStore as "<Name>.<Member>", the same naming scheme FB instance
// members use -- a DB is mechanically the storage-only subset of what FB
// instantiation already needs. Elementary types only in v1.
struct DataBlockAst {
    std::string name;
    std::vector<VarDecl> varDecls;
};

struct StProgramAst {
    std::string name;
    std::vector<VarDecl> varDecls;
    StmtList body;
    // Populated by binding (pou_binder) when the program contains FB instances or FC
    // calls; empty (zero cost) for a program with neither.
    std::vector<Frame> frames;
};

// Everything parsed from one source file: any number of FUNCTION_BLOCK/FUNCTION/
// DATA_BLOCK definitions, plus exactly one PROGRAM (v1: single-file only).
struct CompilationUnit {
    std::vector<PouAst> pous;
    std::vector<DataBlockAst> dataBlocks;
    StProgramAst program;
};

}  // namespace softplc::st
