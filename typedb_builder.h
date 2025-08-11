#pragma once
#include "typedb.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <vector>

namespace me3::typedb {

auto build_type_db(clang::ASTContext &ctx) -> TypeDb;

auto build_type_db(clang::ASTContext &ctx,
                   const std::vector<const clang::CXXRecordDecl *> &records)
    -> TypeDb;

} // namespace me3::typedb