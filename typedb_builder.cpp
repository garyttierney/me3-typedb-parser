#include "typedb_builder.h"
#include "typedb.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/AST/VTableBuilder.h>
#include <clang/Basic/AddressSpaces.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Casting.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace me3::typedb {
namespace {

constexpr uint64_t kBitsPerByte = 8;
constexpr unsigned kWorklistInitialCapacity = 64;
constexpr unsigned kSmallStringBuffer = 32;
constexpr unsigned kDecimalBase = 10;

struct TypeInterner {
  clang::ASTContext *context;
  std::vector<Node> *nodes;
  std::unordered_map<std::string, size_t> index; // id -> node index
  llvm::DenseSet<const clang::CXXRecordDecl *> *seen_records;
  llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
      *worklist;
  clang::PrintingPolicy c_policy;
  TypeInterner(clang::ASTContext &context, std::vector<Node> &all_nodes,
               llvm::DenseSet<const clang::CXXRecordDecl *> &seen,
               llvm::SmallVector<const clang::CXXRecordDecl *,
                                 kWorklistInitialCapacity> &record_worklist)
      : context(&context), nodes(&all_nodes), seen_records(&seen),
        worklist(&record_worklist), c_policy(context.getLangOpts()) {
    c_policy.Bool = true;
    c_policy.SuppressTagKeyword = false;
    c_policy.SuppressScope = false;
    c_policy.SuppressUnwrittenScope = true;
    c_policy.AnonymousTagLocations = false;
  }

  auto as_c_decl(clang::QualType qual_type) const -> std::string {
    clang::QualType canon = qual_type.getCanonicalType();
    return canon.getAsString(c_policy);
  }

  auto intern(Node &&node, const std::string &id_str) -> std::string {
    if (index.contains(id_str)) {
      return id_str;
    }
    Node local = std::move(node);
    local.name = id_str;
    if (local.cdecl.empty()) {
      local.cdecl = id_str;
    }
    nodes->push_back(std::move(local));
    index.emplace(id_str, nodes->size() - 1);
    return id_str;
  }
  auto make_pointer_to(const std::string &pointee) -> std::string {
    std::string id_str = pointee + " *";
    if (index.contains(id_str)) {
      return id_str;
    }
    Node node;
    node.data = PointerType{pointee};
    node.cdecl = id_str;
    return intern(std::move(node), id_str);
  }
  auto get_type_id(clang::QualType original_qt, unsigned depth = 0)
      -> std::string {
    clang::QualType canon = original_qt.getCanonicalType();
    std::string printed = as_c_decl(canon);
    if (const auto *builtin_ty = canon->getAs<clang::BuiltinType>()) {
      std::string spell = as_c_decl(clang::QualType(builtin_ty, 0));
      Node node;
      node.data = BuiltinType{spell};
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *templ_param_ty =
            canon->getAs<clang::TemplateTypeParmType>()) {
      Node node;
      node.data = TemplateParameterType{
          .index = (int)templ_param_ty->getIndex(),
          .depth = (int)templ_param_ty->getDepth(),
          .name = (templ_param_ty->getIdentifier() != nullptr
                       ? templ_param_ty->getIdentifier()->getName().str()
                       : std::string("(anon)"))};
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *ptr_ty = canon->getAs<clang::PointerType>()) {
      clang::QualType pointee_qt = ptr_ty->getPointeeType();
      if (const auto *func_proto =
              pointee_qt->getAs<clang::FunctionProtoType>()) {
        FunctionPointerType fn_ptr_type;
        fn_ptr_type.return_type =
            get_type_id(func_proto->getReturnType(), depth + 1);
        for (clang::QualType param_qt : func_proto->getParamTypes()) {
          fn_ptr_type.params.push_back(get_type_id(param_qt, depth + 1));
        }
        fn_ptr_type.variadic = func_proto->isVariadic();
        Node node;
        node.data = std::move(fn_ptr_type);
        node.cdecl = printed;
        return intern(std::move(node), printed);
      }
      Node node;
      node.data = PointerType{get_type_id(pointee_qt, depth + 1)};
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *lvalue_ref_ty =
            canon->getAs<clang::LValueReferenceType>()) {
      return make_pointer_to(
          get_type_id(lvalue_ref_ty->getPointeeType(), depth + 1));
    }
    if (const auto *rvalue_ref_ty =
            canon->getAs<clang::RValueReferenceType>()) {
      return make_pointer_to(
          get_type_id(rvalue_ref_ty->getPointeeType(), depth + 1));
    }
    if (const auto *const_array_ty = context->getAsConstantArrayType(canon)) {
      Node node;
      node.data = FixedSizeArray{
          .size = const_array_ty->getSize().getZExtValue(),
          .elem = get_type_id(const_array_ty->getElementType(), depth + 1)};
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *incomplete_array_ty =
            context->getAsIncompleteArrayType(canon)) {
      Node node;
      node.data = UnsizedArray{
          get_type_id(incomplete_array_ty->getElementType(), depth + 1)};
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *func_proto_ty = canon->getAs<clang::FunctionProtoType>()) {
      FunctionType function_type;
      function_type.return_type =
          get_type_id(func_proto_ty->getReturnType(), depth + 1);
      for (clang::QualType param_qt : func_proto_ty->getParamTypes()) {
        function_type.params.push_back(get_type_id(param_qt, depth + 1));
      }
      function_type.variadic = func_proto_ty->isVariadic();
      Node node;
      node.data = std::move(function_type);
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *templ_spec_ty =
            canon->getAs<clang::TemplateSpecializationType>()) {
      TemplateSpecializationType spec;
      if (const clang::TemplateDecl *templ_decl =
              templ_spec_ty->getTemplateName().getAsTemplateDecl()) {
        spec.name = templ_decl->getQualifiedNameAsString();
      } else {
        spec.name = printed;
      }
      for (const clang::TemplateArgument &templ_arg :
           templ_spec_ty->template_arguments()) {
        if (templ_arg.getKind() == clang::TemplateArgument::Type) {
          spec.type_args.push_back(
              get_type_id(templ_arg.getAsType(), depth + 1));
        }
      }
      Node node;
      node.data = std::move(spec);
      node.cdecl = printed;
      return intern(std::move(node), printed);
    }
    if (const auto *record_decl = canon->getAsCXXRecordDecl()) {
      std::string record_name;
      if (llvm::isa<clang::ClassTemplateSpecializationDecl>(record_decl)) {
        record_name = as_c_decl(canon);
      } else {
        record_name = record_decl->getQualifiedNameAsString();
      }
      if (record_decl->isCompleteDefinition() &&
          seen_records->insert(record_decl).second) {
        worklist->push_back(record_decl);
      }
      return record_name;
    }
    if (const auto *enum_type = canon->getAs<clang::EnumType>()) {
      const clang::EnumDecl *enum_decl = enum_type->getDecl();
      if (enum_decl->isCompleteDefinition()) {
        return enum_decl->getQualifiedNameAsString();
      }
    }
    Node unknown;
    unknown.data = UnknownType{printed};
    unknown.cdecl = printed;
    return intern(std::move(unknown), printed);
  }
};
} // namespace

namespace {
auto init_db_from_target(const clang::ASTContext &ctx) -> TypeDb {
  TypeDb type_db;
  const clang::TargetInfo &target_info = ctx.getTargetInfo();
  type_db.triple = target_info.getTriple().str();
  type_db.pointer_width_bits =
      static_cast<int>(target_info.getPointerWidth(clang::LangAS::Default));
  type_db.char_width_bits = static_cast<int>(target_info.getCharWidth());
  type_db.long_width_bits = static_cast<int>(target_info.getLongWidth());
  return type_db;
}

void emit_enums(clang::ASTContext &ctx,
                const std::vector<const clang::EnumDecl *> &enums,
                TypeDb &type_db) {
  for (const clang::EnumDecl *enum_decl : enums) {
    if (enum_decl == nullptr || !enum_decl->isCompleteDefinition()) {
      continue;
    }
    std::string name = enum_decl->getQualifiedNameAsString();
    if (type_db.node_index.contains(name)) {
      continue;
    }
    Node node;
    node.name = name;
    EnumType enum_data;
    clang::QualType eqt(enum_decl->getTypeForDecl(), 0);
    enum_data.size_bytes = ctx.getTypeSize(eqt) / kBitsPerByte;
    enum_data.align_bytes = ctx.getTypeAlign(eqt) / kBitsPerByte;
    if (const clang::Type *under_t =
            enum_decl->getIntegerType().getTypePtrOrNull()) {
      enum_data.integer_width_bits = ctx.getTypeSize(under_t);
    }
    for (const clang::EnumConstantDecl *enumerator : enum_decl->enumerators()) {
      llvm::APSInt val = enumerator->getInitVal();
      llvm::SmallString<kSmallStringBuffer> dec_str;
      val.toString(dec_str, kDecimalBase);
      enum_data.enumerators.emplace_back(enumerator->getNameAsString(),
                                         dec_str.str().str());
    }
    node.data = std::move(enum_data);
    type_db.nodes.push_back(std::move(node));
  }
}

void emit_roots_and_templates(
    const std::vector<const clang::CXXRecordDecl *> &roots,
    llvm::DenseSet<const clang::CXXRecordDecl *> &seen_records,
    llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
        &worklist) {
  for (const clang::CXXRecordDecl *root : roots) {
    if (root != nullptr && seen_records.insert(root).second) {
      worklist.push_back(root);
    }
  }
  for (const clang::CXXRecordDecl *root : roots) {
    if (root == nullptr) {
      continue;
    }
    if (const auto *ctd = root->getDescribedClassTemplate()) {
      const clang::CXXRecordDecl *pattern = ctd->getTemplatedDecl();
      if (pattern != nullptr && pattern != root &&
          seen_records.insert(pattern).second) {
        worklist.push_back(pattern);
      }
    }
  }
}

void maybe_queue_record(
    const clang::CXXRecordDecl *rec,
    llvm::DenseSet<const clang::CXXRecordDecl *> &seen_records,
    llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
        &worklist) {
  if (rec != nullptr && rec->isCompleteDefinition() &&
      seen_records.insert(rec).second) {
    worklist.push_back(rec);
  }
}

void emit_vftable_ptrs(clang::ASTContext &ctx, const std::string &record_name,
                       const clang::CXXRecordDecl *record_decl,
                       TypeInterner &interner,
                       std::vector<Node> &synthetic_nodes,
                       std::vector<ObjectField> &fields,
                       ObjectField &vfptr_field_template) {
  if (auto *msvctx = llvm::dyn_cast<clang::MicrosoftVTableContext>(
          ctx.getVTableContext())) {
    uint64_t ptr_bytes =
        ctx.getTargetInfo().getPointerWidth(clang::LangAS::Default) /
        kBitsPerByte;
    unsigned vf_index = 0;
    for (const auto &offset_info : msvctx->getVFPtrOffsets(record_decl)) {
      Node vf_node;
      vf_node.name = record_name + "__vftable_" + std::to_string(vf_index);
      VfTableType table{.original_record = record_name};

      uint64_t slot_index = 0;
      const auto &vt_layout =
          msvctx->getVFTableLayout(record_decl, offset_info->FullOffsetInMDC);
      for (const clang::VTableComponent &component :
           vt_layout.vtable_components()) {
        if (component.getKind() == clang::VTableComponent::CK_FunctionPointer) {
          if (const clang::FunctionDecl *func_decl =
                  component.getFunctionDecl()) {
            ObjectField row;
            std::string fn_name = func_decl->getNameAsString();
            if (fn_name.empty()) {
              fn_name = "fn" + std::to_string(slot_index);
            }
            row.name = fn_name;
            row.size_bytes = ptr_bytes;
            row.type_id = interner.make_pointer_to(
                interner.get_type_id(func_decl->getType()));
            table.fields.push_back(std::move(row));
            ++slot_index;
          }
        }
      }
      table.size_bytes = slot_index * ptr_bytes;
      table.align_bytes = ptr_bytes;
      vf_node.data = std::move(table);
      synthetic_nodes.push_back(std::move(vf_node));
      ObjectField vfptr_field = vfptr_field_template;
      vfptr_field.name = "__vfptr" + std::to_string(vf_index);
      vfptr_field.type_id = interner.make_pointer_to(
          record_name + "__vftable_" + std::to_string(vf_index));
      vfptr_field.size_bytes = ptr_bytes;
      fields.push_back(std::move(vfptr_field));
      ++vf_index;
    }
  }
}

void emit_vftable_type(clang::ASTContext &ctx, const std::string &record_name,
                       const clang::CXXRecordDecl *record_decl,
                       TypeInterner &interner,
                       std::vector<Node> &synthetic_nodes,
                       std::vector<ObjectField> &fields) {
  uint64_t ptr_bytes =
      ctx.getTargetInfo().getPointerWidth(clang::LangAS::Default) /
      kBitsPerByte;
  Node vf_node;
  vf_node.name = record_name + "__vftable_0";
  VfTableType table;
  table.original_record = record_name;
  uint64_t slot_index = 0;
  for (auto *method_decl : record_decl->methods()) {
    if (method_decl == nullptr || !method_decl->isVirtual()) {
      continue;
    }
    ObjectField vf_entry;
    std::string method_name = method_decl->getNameAsString();
    if (method_name.empty()) {
      method_name = "fn" + std::to_string(slot_index);
    }
    vf_entry.name = method_name;
    vf_entry.size_bytes = ptr_bytes;
    vf_entry.type_id =
        interner.make_pointer_to(interner.get_type_id(method_decl->getType()));
    table.fields.push_back(std::move(vf_entry));
    ++slot_index;
  }
  table.size_bytes = slot_index * ptr_bytes;
  table.align_bytes = ptr_bytes;
  vf_node.data = std::move(table);
  synthetic_nodes.push_back(std::move(vf_node));
  ObjectField vfptr_field;
  vfptr_field.name = "__vfptr0";
  vfptr_field.is_vfptr = true;
  vfptr_field.type_id = interner.make_pointer_to(record_name + "__vftable_0");
  vfptr_field.size_bytes = ptr_bytes;
  fields.push_back(std::move(vfptr_field));
}

void build_bases_fields(
    clang::ASTContext &ctx, const clang::CXXRecordDecl *record_decl,
    const clang::ASTRecordLayout *layout, TypeInterner &interner,
    llvm::DenseSet<const clang::CXXRecordDecl *> &seen_records,
    llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
        &worklist,
    std::vector<ObjectField> &fields) {
  for (const auto &base : record_decl->bases()) {
    const clang::CXXRecordDecl *base_decl =
        base.getType()->getAsCXXRecordDecl();
    if (base_decl == nullptr) {
      continue;
    }
    ObjectField base_field;
    base_field.name = base_decl->getQualifiedNameAsString();
    base_field.is_base = true;
    base_field.is_virtual_base = base.isVirtual();
    base_field.type_id = interner.get_type_id(base.getType());
    if ((layout == nullptr) || !base_decl->isCompleteDefinition()) {
      base_field.layout_known = false;
    } else {
      base_field.size_bytes =
          ctx.getASTRecordLayout(base_decl).getSize().getQuantity();
    }
    fields.push_back(std::move(base_field));
    maybe_queue_record(base_decl, seen_records, worklist);
  }
}

void build_member_fields(clang::ASTContext &ctx,
                         const clang::CXXRecordDecl *record_decl,
                         const clang::ASTRecordLayout *layout,
                         TypeInterner &interner,
                         std::vector<ObjectField> &fields) {
  for (const clang::FieldDecl *field_decl : record_decl->fields()) {
    ObjectField member_field;
    member_field.name = field_decl->getNameAsString();
    bool is_dependent =
        field_decl->getType()->isDependentType() || (layout == nullptr);
    if (field_decl->isBitField()) {
      member_field.is_bitfield = true;
      member_field.bit_width = field_decl->getBitWidthValue();
    }
    if (!is_dependent) {
      member_field.size_bytes =
          ctx.getTypeSize(field_decl->getType()) / kBitsPerByte;
    } else {
      member_field.layout_known = false;
    }
    member_field.type_id = interner.get_type_id(field_decl->getType());
    fields.push_back(std::move(member_field));
  }
}

auto build_record_node(
    clang::ASTContext &ctx, const clang::CXXRecordDecl *record_decl,
    TypeInterner &interner,
    llvm::DenseSet<const clang::CXXRecordDecl *> &seen_records,
    llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
        &worklist,
    std::vector<Node> &synthetic_nodes) -> Node {
  Node rec_node;
  ObjectType obj;
  bool is_primary_template =
      record_decl->getDescribedClassTemplate() != nullptr;
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record_decl)) {
    clang::QualType rec_qt = ctx.getRecordType(record_decl);
    clang::PrintingPolicy printer(ctx.getLangOpts());
    printer.SuppressTagKeyword = true;
    rec_node.name = rec_qt.getAsString(printer);
    if (const clang::TemplateDecl *templ_decl =
            spec->getSpecializedTemplate()) {
      if (const auto *ctd =
              llvm::dyn_cast<clang::ClassTemplateDecl>(templ_decl)) {
        const clang::CXXRecordDecl *pattern = ctd->getTemplatedDecl();
        maybe_queue_record(pattern, seen_records, worklist);
        if (pattern != nullptr) {
          std::string base_name = pattern->getQualifiedNameAsString();
          std::string params;
          bool first = true;
          for (const clang::NamedDecl *param_decl :
               *ctd->getTemplateParameters()) {
            if (const auto *type_param =
                    llvm::dyn_cast<clang::TemplateTypeParmDecl>(param_decl)) {
              if (!first) {
                params += ",";
              }
              std::string pname =
                  type_param->getName().empty()
                      ? ("T" + std::to_string(type_param->getIndex()))
                      : type_param->getName().str();
              params += pname;
              first = false;
            }
          }
          obj.primary_template =
              params.empty() ? base_name : base_name + "<" + params + ">";
        }
      }
    }
  } else if (is_primary_template) {
    if (const auto *ctd = record_decl->getDescribedClassTemplate()) {
      std::string base_name = record_decl->getQualifiedNameAsString();
      std::string params;
      bool first = true;
      for (const clang::NamedDecl *param_decl : *ctd->getTemplateParameters()) {
        if (const auto *type_param =
                llvm::dyn_cast<clang::TemplateTypeParmDecl>(param_decl)) {
          if (!first) {
            params += ",";
          }
          std::string pname =
              type_param->getName().empty()
                  ? ("T" + std::to_string(type_param->getIndex()))
                  : type_param->getName().str();
          params += pname;
          clang::QualType param_qt(type_param->getTypeForDecl(), 0);
          obj.template_type_args.push_back(interner.get_type_id(param_qt));
          first = false;
        }
      }
      rec_node.name =
          params.empty() ? base_name : base_name + "<" + params + ">";
    }
  } else {
    rec_node.name = record_decl->getQualifiedNameAsString();
  }
  if (rec_node.name.empty()) {
    rec_node.name = record_decl->getQualifiedNameAsString();
  }
  if (is_primary_template) {
    obj.template_primary = true;
  }
  const clang::ASTRecordLayout *layout = nullptr;
  if (!is_primary_template) {
    layout = &ctx.getASTRecordLayout(record_decl);
    obj.size_bytes = layout->getSize().getQuantity();
    obj.align_bytes = layout->getAlignment().getQuantity();
  } else {
    obj.layout_dependent = true;
  }
  if (const auto *spec =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record_decl)) {
    const clang::TemplateArgumentList &args = spec->getTemplateArgs();
    for (const clang::TemplateArgument &arg : args.asArray()) {
      if (arg.getKind() == clang::TemplateArgument::Type) {
        obj.template_type_args.push_back(interner.get_type_id(arg.getAsType()));
      }
    }
  }
  std::vector<ObjectField> fields;
  build_bases_fields(ctx, record_decl, layout, interner, seen_records, worklist,
                     fields);
  if (is_primary_template && record_decl->isDynamicClass()) {
    emit_vftable_type(ctx, rec_node.name, record_decl, interner,
                      synthetic_nodes, fields);
  }
  if (!is_primary_template && record_decl->isDynamicClass() &&
      record_decl->getNumBases() == 0) {
    ObjectField vfptr_field_template;
    vfptr_field_template.is_vfptr = true;
    emit_vftable_ptrs(ctx, rec_node.name, record_decl, interner,
                      synthetic_nodes, fields, vfptr_field_template);
  }
  build_member_fields(ctx, record_decl, layout, interner, fields);
  obj.fields = std::move(fields);
  rec_node.data = std::move(obj);
  return rec_node;
}

class DbBuildVisitor : public clang::RecursiveASTVisitor<DbBuildVisitor> {
public:
  explicit DbBuildVisitor(clang::ASTContext &ctx)
      : ctx_(&ctx), db_(init_db_from_target(ctx)),
        interner_(ctx, db_.nodes, seen_records_, worklist_) {}

  auto VisitEnumDecl(clang::EnumDecl *decl) -> bool {
    if (decl == nullptr || !decl->isCompleteDefinition()) {
      return true;
    }
    std::string name = decl->getQualifiedNameAsString();
    if (emitted_names_.contains(name)) {
      return true;
    }
    Node node;
    node.name = name;
    EnumType enum_data;
    clang::QualType eqt(decl->getTypeForDecl(), 0);
    enum_data.size_bytes = ctx_->getTypeSize(eqt) / kBitsPerByte;
    enum_data.align_bytes = ctx_->getTypeAlign(eqt) / kBitsPerByte;
    if (const clang::Type *under_t =
            decl->getIntegerType().getTypePtrOrNull()) {
      enum_data.integer_width_bits = ctx_->getTypeSize(under_t);
    }
    for (const clang::EnumConstantDecl *enumerator : decl->enumerators()) {
      llvm::APSInt val = enumerator->getInitVal();
      llvm::SmallString<kSmallStringBuffer> buffer;
      val.toString(buffer, kDecimalBase);
      enum_data.enumerators.emplace_back(enumerator->getNameAsString(),
                                         buffer.str().str());
    }
    node.data = std::move(enum_data);
    db_.nodes.push_back(std::move(node));
    emitted_names_.insert(std::move(name));
    return true;
  }

  auto VisitCXXRecordDecl(clang::CXXRecordDecl *decl) -> bool {
    if (decl == nullptr) {
      return true;
    }
    const bool is_primary_template =
        decl->getDescribedClassTemplate() != nullptr;
    if (!decl->isCompleteDefinition() && !is_primary_template) {
      return true;
    }
    ensure_record_emitted(decl);
    return true;
  }

  void ensure_record_emitted(const clang::CXXRecordDecl *root) {
    if (root == nullptr) {
      return;
    }
    if (!processed_.insert(root).second) {
      return;
    }
    maybe_queue_record(root, seen_records_, worklist_);
    if (const auto *ctd = root->getDescribedClassTemplate()) {
      maybe_queue_record(ctd->getTemplatedDecl(), seen_records_, worklist_);
    }
    std::vector<Node> synthetic_nodes;
    while (!worklist_.empty()) {
      const clang::CXXRecordDecl *record_decl = worklist_.back();
      worklist_.pop_back();
      if (record_decl == nullptr) {
        continue;
      }
      const bool is_primary_template =
          record_decl->getDescribedClassTemplate() != nullptr;
      if (!record_decl->isCompleteDefinition() && !is_primary_template) {
        continue;
      }
      Node rec_node =
          build_record_node(*ctx_, record_decl, interner_, seen_records_,
                            worklist_, synthetic_nodes);
      if (emitted_names_.insert(rec_node.name).second) {
        db_.nodes.push_back(std::move(rec_node));
      }
    }
    for (auto &synthetic : synthetic_nodes) {
      if (emitted_names_.insert(synthetic.name).second) {
        db_.nodes.push_back(std::move(synthetic));
      }
    }
  }

  auto build() -> TypeDb {
    db_.build_indices();
    return std::move(db_);
  }

private:
  clang::ASTContext *ctx_;
  TypeDb db_;
  llvm::DenseSet<const clang::CXXRecordDecl *> seen_records_;
  llvm::DenseSet<const clang::CXXRecordDecl *> processed_;
  llvm::SmallVector<const clang::CXXRecordDecl *, kWorklistInitialCapacity>
      worklist_;
  TypeInterner interner_;
  std::unordered_set<std::string> emitted_names_;
};
} // namespace

auto build_type_db(clang::ASTContext &ctx) -> TypeDb {
  DbBuildVisitor visitor(ctx);
  visitor.TraverseDecl(ctx.getTranslationUnitDecl());
  return visitor.build();
}

} // namespace me3::typedb
