#include "softplc/st/pou_binder.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace softplc::st {

namespace {

// One instance (FB) or call-site (FC) a caller's body can invoke: the callee's own
// POU definition (for VAR_INPUT/VAR_OUTPUT kind lookup during arg validation), the
// Frame its bound body lives in, and its own member-name -> TagId scope (used to
// resolve CallArg::paramName to the callee's concrete tag).
struct CallableInfo {
    const PouAst* pouDef = nullptr;
    std::size_t frameIndex = kInvalidFrame;
    std::unordered_map<std::string, tags::TagId> paramScope;
};

// Threaded through the whole recursive bind, one instance per compilation unit.
struct BinderContext {
    tags::TagStore& tags;
    const std::unordered_map<std::string, const PouAst*>& registry;
    std::vector<Frame> frames;
    // POU names currently being expanded, for cycle detection (an FB/FC that
    // (in)directly instantiates/calls itself has no well-defined clone-and-rebind
    // output).
    std::vector<std::string> expandingTypes;
    int fcCallCounter = 0;
};

struct ExpansionResult {
    // Bare member name (possibly dotted for a nested instance's members, e.g.
    // "TmpTimer.Q") -> this expansion's concrete TagId. Returned so a caller
    // instantiating this POU can fold these in under a further prefix.
    std::unordered_map<std::string, tags::TagId> localScope;
    // Index into BinderContext::frames of this expansion's own bound body.
    std::size_t frameIndexOfSelf = kInvalidFrame;
};

struct DeclResult {
    std::unordered_map<std::string, tags::TagId> localScope;
    std::unordered_map<std::string, CallableInfo> instanceCallables;
};

tags::TagId resolveName(const std::string& name,
                         const std::unordered_map<std::string, tags::TagId>& localScope,
                         tags::TagStore& tags) {
    auto it = localScope.find(name);
    if (it != localScope.end()) return it->second;
    // Falls back to a direct TagStore lookup -- this is how DATA_BLOCK members
    // ("DB1.Speed") resolve: they're global storage, not part of any POU's local
    // scope, declared once up front by bindCompilationUnit().
    auto found = tags.find(name);
    if (!found) {
        throw std::runtime_error("undeclared identifier: " + name);
    }
    return *found;
}

ExpansionResult expandPou(const PouAst& pou, const std::string& tagPrefix, BinderContext& ctx);

DeclResult declareVars(const std::vector<VarDecl>& varDecls, const std::string& tagPrefix,
                        BinderContext& ctx) {
    DeclResult result;
    for (const auto& decl : varDecls) {
        const std::string tagName = tagPrefix.empty() ? decl.name : tagPrefix + "." + decl.name;

        if (decl.elementaryType) {
            tags::Value initial =
                decl.initialValue ? *decl.initialValue : tags::defaultValueFor(*decl.elementaryType);
            const tags::TagId id =
                ctx.tags.declare(tagName, *decl.elementaryType, std::move(initial), decl.address);
            result.localScope[decl.name] = id;
        } else if (decl.instanceOfType) {
            auto it = ctx.registry.find(*decl.instanceOfType);
            if (it == ctx.registry.end()) {
                throw std::runtime_error("undeclared function block type: " + *decl.instanceOfType);
            }
            const PouAst* nestedPou = it->second;

            if (std::find(ctx.expandingTypes.begin(), ctx.expandingTypes.end(),
                           *decl.instanceOfType) != ctx.expandingTypes.end()) {
                throw std::runtime_error("circular function block instantiation involving " +
                                          *decl.instanceOfType);
            }
            ctx.expandingTypes.push_back(*decl.instanceOfType);
            ExpansionResult nested = expandPou(*nestedPou, tagName, ctx);
            ctx.expandingTypes.pop_back();

            for (const auto& [nestedKey, nestedId] : nested.localScope) {
                result.localScope[decl.name + "." + nestedKey] = nestedId;
            }
            result.instanceCallables[decl.name] =
                CallableInfo{nestedPou, nested.frameIndexOfSelf, std::move(nested.localScope)};
        } else {
            throw std::runtime_error("internal error: VarDecl '" + decl.name +
                                      "' has neither an elementary type nor an instance type");
        }
    }
    return result;
}

void bindExpr(Expr& expr, const std::unordered_map<std::string, tags::TagId>& localScope,
              BinderContext& ctx) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return;
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(expr);
            id.tagId = resolveName(id.name, localScope, ctx.tags);
            return;
        }
        case ExprKind::Unary:
            bindExpr(*static_cast<UnaryExpr&>(expr).operand, localScope, ctx);
            return;
        case ExprKind::Binary: {
            auto& binary = static_cast<BinaryExpr&>(expr);
            bindExpr(*binary.lhs, localScope, ctx);
            bindExpr(*binary.rhs, localScope, ctx);
            return;
        }
    }
}

void bindStmtListInto(StmtList& stmts, const std::unordered_map<std::string, tags::TagId>& localScope,
                       const std::unordered_map<std::string, CallableInfo>& instanceCallables,
                       BinderContext& ctx);

void bindCallStmt(CallStmt& call, const std::unordered_map<std::string, tags::TagId>& callerScope,
                   const std::unordered_map<std::string, CallableInfo>& instanceCallables,
                   BinderContext& ctx) {
    const CallableInfo* info = nullptr;
    std::optional<CallableInfo> fcInfoStorage;

    auto instIt = instanceCallables.find(call.calleeName);
    if (instIt != instanceCallables.end()) {
        info = &instIt->second;
    } else {
        auto regIt = ctx.registry.find(call.calleeName);
        if (regIt == ctx.registry.end()) {
            throw std::runtime_error("call to undeclared instance or function: " + call.calleeName);
        }
        const PouAst* calleePou = regIt->second;
        if (!calleePou->isFunction) {
            throw std::runtime_error(call.calleeName +
                                      " is a FUNCTION_BLOCK type; declare an instance of it in a "
                                      "VAR block before calling it");
        }
        if (std::find(ctx.expandingTypes.begin(), ctx.expandingTypes.end(), call.calleeName) !=
            ctx.expandingTypes.end()) {
            throw std::runtime_error("circular function call involving " + call.calleeName);
        }
        ctx.expandingTypes.push_back(call.calleeName);
        const std::string prefix = "$fc_" + call.calleeName + "_" + std::to_string(ctx.fcCallCounter++);
        ExpansionResult fcResult = expandPou(*calleePou, prefix, ctx);
        ctx.expandingTypes.pop_back();

        fcInfoStorage = CallableInfo{calleePou, fcResult.frameIndexOfSelf, std::move(fcResult.localScope)};
        info = &*fcInfoStorage;
    }

    call.frameIndex = info->frameIndex;

    // Tracks which required (VAR_INPUT) params of a FUNCTION were bound, so an
    // omitted one is a load-time error (unlike FB, where an unbound input just
    // keeps its last value).
    std::unordered_map<std::string, bool> requiredInputsBound;
    if (info->pouDef->isFunction) {
        for (const auto& decl : info->pouDef->varDecls) {
            if (decl.kind == VarKind::VarInput) requiredInputsBound[decl.name] = false;
        }
    }

    for (auto& arg : call.args) {
        const VarDecl* paramDecl = nullptr;
        for (const auto& decl : info->pouDef->varDecls) {
            if (decl.name == arg.paramName &&
                (decl.kind == VarKind::VarInput || decl.kind == VarKind::VarOutput)) {
                paramDecl = &decl;
                break;
            }
        }
        if (!paramDecl) {
            throw std::runtime_error("unknown parameter '" + arg.paramName + "' in call to " +
                                      call.calleeName);
        }
        if (arg.isOutput && paramDecl->kind != VarKind::VarOutput) {
            throw std::runtime_error("'" + arg.paramName + "' is not a VAR_OUTPUT of " + call.calleeName +
                                      " (used with '=>')");
        }
        if (!arg.isOutput && paramDecl->kind != VarKind::VarInput) {
            throw std::runtime_error("'" + arg.paramName + "' is not a VAR_INPUT of " + call.calleeName +
                                      " (used with ':=')");
        }

        auto paramIt = info->paramScope.find(arg.paramName);
        if (paramIt == info->paramScope.end()) {
            throw std::runtime_error("internal error: parameter '" + arg.paramName +
                                      "' has no bound tag in " + call.calleeName);
        }
        arg.paramTagId = paramIt->second;

        if (arg.isOutput) {
            arg.outputTargetId = resolveName(arg.outputTargetName, callerScope, ctx.tags);
        } else {
            bindExpr(*arg.inputExpr, callerScope, ctx);
            auto reqIt = requiredInputsBound.find(arg.paramName);
            if (reqIt != requiredInputsBound.end()) reqIt->second = true;
        }
    }

    if (info->pouDef->isFunction) {
        for (const auto& [name, bound] : requiredInputsBound) {
            if (!bound) {
                throw std::runtime_error("call to FUNCTION " + call.calleeName +
                                          " is missing required input '" + name + "'");
            }
        }
    }
}

void bindStmt(Stmt& stmt, const std::unordered_map<std::string, tags::TagId>& localScope,
              const std::unordered_map<std::string, CallableInfo>& instanceCallables,
              BinderContext& ctx) {
    switch (stmt.kind) {
        case StmtKind::Assign: {
            auto& assign = static_cast<AssignStmt&>(stmt);
            assign.targetId = resolveName(assign.target, localScope, ctx.tags);
            bindExpr(*assign.value, localScope, ctx);
            return;
        }
        case StmtKind::If: {
            auto& ifStmt = static_cast<IfStmt&>(stmt);
            for (auto& branch : ifStmt.branches) {
                bindExpr(*branch.condition, localScope, ctx);
                bindStmtListInto(branch.body, localScope, instanceCallables, ctx);
            }
            bindStmtListInto(ifStmt.elseBody, localScope, instanceCallables, ctx);
            return;
        }
        case StmtKind::While: {
            auto& whileStmt = static_cast<WhileStmt&>(stmt);
            bindExpr(*whileStmt.condition, localScope, ctx);
            bindStmtListInto(whileStmt.body, localScope, instanceCallables, ctx);
            return;
        }
        case StmtKind::Call:
            bindCallStmt(static_cast<CallStmt&>(stmt), localScope, instanceCallables, ctx);
            return;
        case StmtKind::Rung: {
            auto& rung = static_cast<RungStmt&>(stmt);
            bindExpr(*rung.condition, localScope, ctx);
            for (auto& out : rung.outputs) {
                out.targetId = resolveName(out.targetName, localScope, ctx.tags);
                if (ctx.tags.typeOf(out.targetId) != tags::TypeId::Bool) {
                    throw std::runtime_error("RUNG output '" + out.targetName +
                                              "' must be a BOOL tag (coils are boolean outputs)");
                }
            }
            return;
        }
    }
}

void bindStmtListInto(StmtList& stmts, const std::unordered_map<std::string, tags::TagId>& localScope,
                       const std::unordered_map<std::string, CallableInfo>& instanceCallables,
                       BinderContext& ctx) {
    for (auto& stmt : stmts) {
        bindStmt(*stmt, localScope, instanceCallables, ctx);
    }
}

ExpansionResult expandPou(const PouAst& pou, const std::string& tagPrefix, BinderContext& ctx) {
    DeclResult decls = declareVars(pou.varDecls, tagPrefix, ctx);

    StmtList boundBody = cloneStmtList(pou.body);
    bindStmtListInto(boundBody, decls.localScope, decls.instanceCallables, ctx);

    Frame frame;
    for (const auto& decl : pou.varDecls) {
        if (decl.kind == VarKind::VarTemp && decl.elementaryType) {
            const tags::TagId id = decls.localScope.at(decl.name);
            tags::Value resetValue =
                decl.initialValue ? *decl.initialValue : tags::defaultValueFor(*decl.elementaryType);
            frame.tempResets.emplace_back(id, std::move(resetValue));
        }
    }
    frame.body = std::move(boundBody);
    ctx.frames.push_back(std::move(frame));

    ExpansionResult result;
    result.frameIndexOfSelf = ctx.frames.size() - 1;
    result.localScope = std::move(decls.localScope);
    return result;
}

StProgramAst bindProgram(StProgramAst program, BinderContext& ctx) {
    DeclResult decls = declareVars(program.varDecls, "", ctx);
    bindStmtListInto(program.body, decls.localScope, decls.instanceCallables, ctx);
    program.frames = std::move(ctx.frames);
    return program;
}

}  // namespace

StProgramAst bindCompilationUnit(CompilationUnit unit, tags::TagStore& tags) {
    // A well-known global TIME tag, written by ScanEngine every scan with the actual
    // elapsed time since the previous scan started. Any POU body can reference it by
    // name (resolveName()'s TagStore fallback, same mechanism DATA_BLOCK members use)
    // without declaring it themselves -- this is what lets TON/TOF/CTU/CTD (see
    // st/standard_fbs.hpp) be ordinary ST-defined FUNCTION_BLOCKs rather than
    // needing native C++ intrinsics.
    tags.declare("System.CycleTime", tags::TypeId::Time, tags::TimeValue{0});

    std::unordered_map<std::string, const PouAst*> registry;
    for (const auto& pou : unit.pous) {
        if (!registry.emplace(pou.name, &pou).second) {
            throw std::runtime_error("duplicate FUNCTION_BLOCK/FUNCTION name: " + pou.name);
        }
    }

    for (const auto& db : unit.dataBlocks) {
        for (const auto& decl : db.varDecls) {
            if (!decl.elementaryType) {
                throw std::runtime_error("DATA_BLOCK '" + db.name +
                                          "': only elementary-typed members are supported");
            }
            tags::Value initial =
                decl.initialValue ? *decl.initialValue : tags::defaultValueFor(*decl.elementaryType);
            tags.declare(db.name + "." + decl.name, *decl.elementaryType, std::move(initial), decl.address);
        }
    }

    BinderContext ctx{tags, registry};
    return bindProgram(std::move(unit.program), ctx);
}

}  // namespace softplc::st
