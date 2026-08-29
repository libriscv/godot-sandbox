#pragma once
#include "ast.h"

namespace gdscript {

ExprPtr clone_expr(const Expr* expr);
StmtPtr clone_stmt(const Stmt* stmt);
MatchPatternPtr clone_pattern(const MatchPattern* pattern);
Parameter clone_parameter(const Parameter& parameter);
FunctionDecl clone_function(const FunctionDecl& function);
VarDeclStmt clone_var(const VarDeclStmt& variable);
StructField clone_field(const StructField& field);
SignalDecl clone_signal(const SignalDecl& signal);
EnumDecl clone_enum(const EnumDecl& value);

} // namespace gdscript
