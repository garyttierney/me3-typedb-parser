#pragma once
#include "typedb.h"
#include <llvm/Support/JSON.h>

namespace me3::typedb {

auto typedb_to_json(const TypeDb &type_db) -> llvm::json::Value;

}