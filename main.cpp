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

auto main(int argc, const char **argv) -> int {
  namespace cl = llvm::cl;

  cl::OptionCategory options_cateogry("dump-layouts options");
  cl::list<std::string> options_extra_args(
      "extra-arg", cl::desc("Additional compile argument (can be repeated)"),
      cl::ZeroOrMore, options_cateogry);

  const cl::opt<std::string> SourcePath(cl::Positional,
                                        cl::desc("<source-file>"), cl::Required,
                                        options_cateogry);
  cl::HideUnrelatedOptions(options_cateogry);
  if (cl::ParseCommandLineOptions(argc, argv, "Dump record layouts\n")) {
    std::vector<std::string> CompileArgs = {
        "-std=c++17", "--target=x86_64-pc-windows-msvc", "-O0", "-g"};
    CompileArgs.insert(CompileArgs.end(), options_extra_args.begin(),
                       options_extra_args.end());

    FixedCompilationDatabase const Compilations(".", CompileArgs);

    std::vector<std::string> const Sources{SourcePath};
    ClangTool Tool(Compilations, Sources);

    return Tool.run(
        newFrontendActionFactory<me3::typedb::CreateTypeDbAction>().get());
  }
  return 1;
}