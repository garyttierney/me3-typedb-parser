#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
#include <vector>

#include "typedb.h"
#include "typedb_builder.h"
#include "typedb_json.h"

using namespace clang;
using namespace clang::tooling;

namespace me3::typedb {
class TypeDbAstConsumer : public ASTConsumer {
public:
  void HandleTranslationUnit(ASTContext &ctx) override {
    TypeDb const m = build_type_db(ctx);
    llvm::outs() << llvm::formatv("{0:2}\n", typedb_to_json(m));
  }
};

class CreateTypeDbAction : public ASTFrontendAction {
public:
  auto CreateASTConsumer(CompilerInstance & /*CI*/, llvm::StringRef /*InFile*/)
      -> std::unique_ptr<ASTConsumer> override {
    return std::make_unique<TypeDbAstConsumer>();
  }
};
} // namespace me3::typedb

static llvm::cl::OptionCategory CLI_CATEGORY("dump-layouts options");

static llvm::cl::list<std::string> CLI_EXTRA_ARGS(
    "extra-arg",
    llvm::cl::desc("Additional compile argument (can be repeated)"),
    llvm::cl::ZeroOrMore, llvm::cl::cat(CLI_CATEGORY));

auto main(int argc, const char **argv) -> int {
  namespace cl = llvm::cl;
  cl::opt<std::string> const SourcePath(cl::Positional,
                                        cl::desc("<source-file>"), cl::Required,
                                        cl::cat(CLI_CATEGORY));
  cl::HideUnrelatedOptions(CLI_CATEGORY);
  if (cl::ParseCommandLineOptions(argc, argv, "Dump record layouts\n")) {
    std::vector<std::string> CompileArgs = {
        "-std=c++17", "--target=x86_64-pc-windows-msvc", "-O0", "-g"};
    CompileArgs.insert(CompileArgs.end(), CLI_EXTRA_ARGS.begin(),
                       CLI_EXTRA_ARGS.end());

    FixedCompilationDatabase const Compilations(".", CompileArgs);

    std::vector<std::string> const Sources{SourcePath};
    ClangTool Tool(Compilations, Sources);

    return Tool.run(
        newFrontendActionFactory<me3::typedb::CreateTypeDbAction>().get());
  }
  return 1;
}