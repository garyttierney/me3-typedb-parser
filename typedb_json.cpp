#include "typedb_json.h"

namespace me3::typedb {

static auto field_json(const ObjectField &field) -> llvm::json::Object {
  llvm::json::Object field_obj;
  if (field.is_base) {
    field_obj["kind"] = "base";
  } else if (field.is_vfptr) {
    field_obj["kind"] = "vfptr";
  } else if (field.is_bitfield) {
    field_obj["kind"] = "bitfield";
  } else {
    field_obj["kind"] = "field";
  }
  field_obj["name"] = field.name;
  if (field.is_virtual_base) {
    field_obj["is_virtual_base"] = true;
  }
  if (field.is_bitfield && field.bit_width) {
    field_obj["bit_width"] = *field.bit_width;
  }
  if (field.layout_known && field.size_bytes != 0) {
    field_obj["size_bytes"] = field.size_bytes;
  }
  field_obj["type"] = field.type_id;
  return field_obj;
}

namespace {
struct NodeJsonVisitor {
  static auto to_array(const std::vector<std::string> &values)
      -> llvm::json::Array {
    llvm::json::Array json_array;
    json_array.reserve(values.size());
    for (auto const &value : values) {
      json_array.push_back(value);
    }
    return json_array;
  }
  static auto make_function_like(const char *kind, const std::string &ret,
                                 const std::vector<std::string> &params,
                                 bool variadic) -> llvm::json::Object {
    llvm::json::Object object;
    object["kind"] = kind;
    object["return_type"] = ret;
    object["params"] = to_array(params);
    if (variadic) {
      object["variadic"] = true;
    }
    return object;
  }
  auto operator()(const BuiltinType &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "builtin";
    result["name"] = value.name;
    return result;
  }
  auto operator()(const TemplateParameterType &value) const
      -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "template_param";
    result["index"] = value.index;
    result["depth"] = value.depth;
    result["name"] = value.name;
    return result;
  }
  auto operator()(const PointerType &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "pointer";
    result["pointee"] = value.pointee;
    return result;
  }
  auto operator()(const FixedSizeArray &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "const_array";
    result["size"] = value.size;
    result["elem"] = value.elem;
    return result;
  }
  auto operator()(const UnsizedArray &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "incomplete_array";
    result["elem"] = value.elem;
    return result;
  }
  auto operator()(const FunctionType &value) const -> llvm::json::Object {
    return make_function_like("function", value.return_type, value.params,
                              value.variadic);
  }
  auto operator()(const FunctionPointerType &value) const
      -> llvm::json::Object {
    return make_function_like("function_pointer", value.return_type,
                              value.params, value.variadic);
  }
  auto operator()(const TemplateSpecializationType &value) const
      -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "template_specialization";
    result["name"] = value.name;
    result["type_args"] = to_array(value.type_args);
    return result;
  }
  auto operator()(const ObjectType &value) const -> llvm::json::Object {
    llvm::json::Object object;
    if (value.template_primary) {
      object["template_primary"] = true;
    }
    if (value.primary_template) {
      object["primary_template"] = *value.primary_template;
    }
    if (value.layout_dependent) {
      object["layout_dependent"] = true;
      if (value.size_bytes != 0) {
        object["size_bytes"] = value.size_bytes;
      }
      if (value.align_bytes != 0) {
        object["align_bytes"] = value.align_bytes;
      }
    } else {
      object["size_bytes"] = value.size_bytes;
      object["align_bytes"] = value.align_bytes;
    }
    if (!value.template_type_args.empty()) {
      object["template_type_args"] = to_array(value.template_type_args);
    }
    llvm::json::Array field_array;
    field_array.reserve(value.fields.size());
    for (auto const &field : value.fields) {
      field_array.push_back(field_json(field));
    }
    object["fields"] = std::move(field_array);
    return object;
  }
  auto operator()(const EnumType &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "enum";
    result["size_bytes"] = value.size_bytes;
    result["align_bytes"] = value.align_bytes;
    result["integer_width_bits"] = value.integer_width_bits;
    if (!value.enumerators.empty()) {
      llvm::json::Array enumerator_array;
      enumerator_array.reserve(value.enumerators.size());
      for (auto const &enumerator : value.enumerators) {
        llvm::json::Object enumerator_object;
        enumerator_object["name"] = enumerator.first;
        enumerator_object["value"] = enumerator.second;
        enumerator_array.push_back(std::move(enumerator_object));
      }
      result["enumerators"] = std::move(enumerator_array);
    }
    return result;
  }
  auto operator()(const VfTableType &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "vftable";
    result["synthetic"] = true;
    result["original_record"] = value.original_record;
    result["size_bytes"] = value.size_bytes;
    result["align_bytes"] = value.align_bytes;
    llvm::json::Array rows;
    rows.reserve(value.fields.size());
    for (auto const &field : value.fields) {
      rows.push_back(field_json(field));
    }
    result["fields"] = std::move(rows);
    return result;
  }
  auto operator()(const UnknownType &value) const -> llvm::json::Object {
    llvm::json::Object result;
    result["kind"] = "unknown";
    result["spelling"] = value.spelling;
    return result;
  }
};
} // namespace

static auto node_payload(const Node &node) -> llvm::json::Object {
  llvm::json::Object payload = std::visit(NodeJsonVisitor{}, node.data);
  return payload;
}

auto typedb_to_json(const TypeDb &db) -> llvm::json::Value {
  llvm::json::Object root;
  root["schema_version"] = SCHEMA_VERSION;
  root["triple"] = db.triple;
  root["pointer_width_bits"] = db.pointer_width_bits;
  root["char_width_bits"] = db.char_width_bits;
  root["long_width_bits"] = db.long_width_bits;
  llvm::json::Object nodes_obj;
  for (auto const &node : db.nodes) {
    llvm::json::Object payload = node_payload(node);
    if (!node.cdecl.empty()) {
      payload["cdecl"] = node.cdecl;
    }
    nodes_obj[node.name] = std::move(payload);
  }
  root["nodes"] = std::move(nodes_obj);
  return llvm::json::Value(std::move(root));
}

} // namespace me3::typedb
