#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace me3::typedb {

struct BuiltinType {
  std::string name;
};

struct TemplateParameterType {
  int index;
  int depth;
  std::string name;
};

struct PointerType {
  std::string pointee;
};

struct FixedSizeArrayType {
  uint64_t size;
  std::string elem;
};

struct UnsizedArrayType {
  std::string elem;
};

struct FunctionType {
  std::string return_type;
  std::vector<std::string> params;
  bool variadic = false;
};

struct TemplateSpecializationType {
  std::string name;
  std::vector<std::string> type_args;
};

struct ObjectField;
struct ObjectType {
  uint64_t size_bytes = 0;
  uint64_t align_bytes = 0;
  bool template_primary = false;
  bool layout_dependent = false;
  std::vector<std::string> template_type_args;
  std::optional<std::string> primary_template;
  std::vector<ObjectField> fields;
};

struct EnumType {
  uint64_t size_bytes = 0;
  uint64_t align_bytes = 0;
  std::string underlying_type;
  std::vector<std::pair<std::string, std::string>> enumerators;
};

struct VfTableType {
  std::string original_record;
  uint64_t size_bytes = 0;
  uint64_t align_bytes = 0;
  std::vector<ObjectField> fields;
};

struct UnknownType {
  std::string spelling;
};

struct ObjectField {
  std::string name;
  uint64_t size_bytes = 0;
  std::optional<uint64_t> bit_width; // iff bitfield
  bool is_base = false;
  bool is_virtual_base = false;
  bool is_vfptr = false;
  bool is_bitfield = false;
  std::string type_id;
  bool layout_known = true;
};

using NodeVariant =
    std::variant<BuiltinType, TemplateParameterType, PointerType,
                 FixedSizeArrayType, UnsizedArrayType, FunctionType,
                 TemplateSpecializationType, ObjectType, EnumType, VfTableType,
                 UnknownType>;

struct Node {
  std::string name;
  NodeVariant data;
  std::string cdecl;
};

struct TypeDb {
  std::vector<Node> nodes;
  std::unordered_map<std::string, size_t> node_index;
  std::string triple;
  int pointer_width_bits = 0;
  int char_width_bits = 0;
  int long_width_bits = 0;
  void build_indices() {
    node_index.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
      node_index.emplace(nodes[i].name, i);
    }
  }
};

inline constexpr const char *SCHEMA_VERSION = "5.0.0";
} // namespace me3::typedb