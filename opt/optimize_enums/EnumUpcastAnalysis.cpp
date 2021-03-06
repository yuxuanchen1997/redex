/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumUpcastAnalysis.h"

#include "Walkers.h"

namespace {

using namespace optimize_enums;
using namespace ir_analyzer;

bool need_analyze(const DexMethod* method,
                  const ConcurrentSet<DexType*>& candidate_enums,
                  const ConcurrentSet<DexType*>& rejected_enums) {
  const IRCode* code = method->get_code();
  if (!code) {
    return false;
  }
  std::vector<DexType*> types;
  method->get_proto()->gather_types(types);
  method->gather_types(types);
  for (DexType* t : types) {
    if (is_array(t)) {
      t = get_array_type(t);
    }
    if (candidate_enums.count_unsafe(t) && !rejected_enums.count(t)) {
      return true;
    }
  }
  return false;
}

std::unordered_set<DexType*> discard_primitives(const EnumTypes& types) {
  std::unordered_set<DexType*> res;
  for (auto type : types.elements()) {
    if (!is_primitive(type)) {
      res.insert(type);
    }
  }
  return res;
}

/**
 * The reason why an enum can not be converted to Integer object.
 * We can figure out more possible optimizations based on the logged reasons and
 * may be able to refactor Java code to optimize more enums.
 * Note: Some enums may be rejected by multiple reasons and we don't log all of
 * them.
 */
enum Reason {
  UNKNOWN,
  CAST_WHEN_RETURN,
  CAST_THIS_POINTER,
  CAST_PARAMETER,
  USED_AS_CLASS_OBJECT,
  CAST_CHECK_CAST,
  CAST_ISPUT_OBJECT,
  CAST_APUT_OBJECT
};

/**
 * Inspect instructions to reject enum class that may be casted to another type.
 */
class EnumUpcastDetector {
 public:
  EnumUpcastDetector(const DexMethod* method,
                     const ConcurrentSet<DexType*>* candidate_enums)
      : m_method(method), m_candidate_enums(candidate_enums) {}

  void run(const EnumFixpointIterator& engine,
           const cfg::ControlFlowGraph& cfg,
           ConcurrentSet<DexType*>* rejected_enums) {
    for (const auto& block : cfg.blocks()) {
      EnumTypeEnvironment env = engine.get_entry_state_at(block);
      if (env.is_bottom()) {
        continue;
      }
      for (const auto& mie : InstructionIterable(block)) {
        engine.analyze_instruction(mie.insn, &env);
        process_instruction(mie.insn, &env, rejected_enums);
      }
    }
  }

 private:
  /**
   * Process instructions when we reach the fixpoint.
   */
  void process_instruction(const IRInstruction* insn,
                           const EnumTypeEnvironment* env,
                           ConcurrentSet<DexType*>* rejected_enums) {
    switch (insn->opcode()) {
    case OPCODE_CHECK_CAST:
      reject_if_inconsistent(env->get(insn->src(0)), insn->get_type(),
                             rejected_enums, CAST_CHECK_CAST);
      break;
    case OPCODE_CONST_CLASS:
      reject(insn->get_type(), rejected_enums, USED_AS_CLASS_OBJECT);
      break;
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_VIRTUAL:
      process_invoke(insn, env, rejected_enums);
      break;
    case OPCODE_RETURN_OBJECT:
      process_return_object(insn, env, rejected_enums);
      break;
    case OPCODE_APUT_OBJECT:
      process_aput_object(insn, env, rejected_enums);
      break;
    case OPCODE_IGET_OBJECT:
      // Candidate enums do not contain any instance field.
      always_assert(
          !m_candidate_enums->count_unsafe(insn->get_field()->get_class()));
      break;
    case OPCODE_IPUT_OBJECT:
      always_assert(
          !m_candidate_enums->count_unsafe(insn->get_field()->get_class()));
      process_isput_object(insn, env, rejected_enums);
      break;
    case OPCODE_SPUT_OBJECT:
      process_isput_object(insn, env, rejected_enums);
      break;
    default:
      break;
    }
  }

  /**
   * Process return-object instruction when we reach the fixpoint.
   */
  void process_return_object(const IRInstruction* insn,
                             const EnumTypeEnvironment* env,
                             ConcurrentSet<DexType*>* rejected_enums) const {
    DexType* return_type = m_method->get_proto()->get_rtype();
    always_assert_log(env->get(insn->src(0)).is_value(),
                      "method %s\ninsn %s %s\n", SHOW(m_method), SHOW(insn),
                      SHOW(m_method->get_code()->cfg()));
    reject_if_inconsistent(env->get(insn->src(0)), return_type, rejected_enums,
                           CAST_WHEN_RETURN);
  }

  /**
   * Process iput-object and sput-object instructions when we reach the fix
   * point.
   */
  void process_isput_object(const IRInstruction* insn,
                            const EnumTypeEnvironment* env,
                            ConcurrentSet<DexType*>* rejected_enums) const {
    auto arg_reg = insn->src(0);
    DexType* field_type = insn->get_field()->get_type();
    reject_if_inconsistent(env->get(arg_reg), field_type, rejected_enums,
                           CAST_ISPUT_OBJECT);
  }

  /**
   * Process aput-object instruction when we reach the fixpoint.
   */
  void process_aput_object(const IRInstruction* insn,
                           const EnumTypeEnvironment* env,
                           ConcurrentSet<DexType*>* rejected_enums) const {
    // It's possible that the array_types contains non-array types or is
    // array of primitives. Just ignore them.
    EnumTypes array_types = env->get(insn->src(1));
    EnumTypes elem_types = env->get(insn->src(0));
    std::unordered_set<DexType*> acceptable_elem_types;
    for (DexType* type : array_types.elements()) {
      DexType* elem = get_array_type(type);
      if (elem && !is_primitive(elem)) {
        acceptable_elem_types.insert(elem); // An array of one type of objects.
      }
    }
    if (acceptable_elem_types.size() > 1) {
      // If a register might be an array of multiple types, it's hard to do
      // further analysis so that we simply reject the types here.
      reject(elem_types, rejected_enums, CAST_APUT_OBJECT);
      reject(acceptable_elem_types, rejected_enums, CAST_APUT_OBJECT);
    } else if (acceptable_elem_types.size() == 1) {
      DexType* acceptable = *acceptable_elem_types.begin();
      reject_if_inconsistent(elem_types, acceptable, rejected_enums,
                             CAST_APUT_OBJECT);
    }
  }

  /**
   * Process invoke-kind instructions after we reach the fixpoint.
   * Analyze invoke instruction's arguments, if the type of arguments are not
   * consistent with the method signature, reject these types.
   *
   * But we can make assumptions for some methods although the invocations
   * seem to involve some cast operations.
   *
   *  # Enum.equals(Object) and Enum.compareTo(Enum) are final methods.
   *  INVOKE_VIRTUAL LCandidateEnum;.equals:(Ljava/lang/Object;)Z
   *  INVOKE_VIRTUAL LCandidateEnum;.compareTo:(Ljava/lang/Enum;)I
   *
   *  # We reject the candidate enum if it overrides `toString()` previously,
   *  # so the CandidateEnum.toString() is Enum.toString() and it behaves
   *  # the same as CandidateEnum.name().
   *  INVOKE_VIRTUAL LCandidateEnum;.toString:()String
   *  INVOKE_VIRTUAL LCandidateEnum;.name:()String
   *
   *  # When the Object param is a candidate enum object, the invocation can be
   *  * modeled.
   *  INVOKE_VIRTUAL StringBuilder.append:(Object)StringBuilder
   */
  void process_invoke(const IRInstruction* insn,
                      const EnumTypeEnvironment* env,
                      ConcurrentSet<DexType*>* rejected_enums) const {
    const DexMethodRef* method = insn->get_method();
    const DexProto* proto = method->get_proto();
    DexType* container = method->get_class();

    // Method is equals or compareTo.
    if (signatures_match(method, ENUM_EQUALS_METHOD) ||
        signatures_match(method, ENUM_COMPARETO_METHOD)) {
      // Class is Enum or a candidate enum class.
      if (container == ENUM_TYPE ||
          (m_candidate_enums->count_unsafe(container) &&
           !rejected_enums->count(container))) {
        EnumTypes a_types = env->get(insn->src(0));
        EnumTypes b_types = env->get(insn->src(1));
        auto this_types = discard_primitives(a_types);
        auto that_types = discard_primitives(b_types);
        DexType* this_type = this_types.empty() ? nullptr : *this_types.begin();
        DexType* that_type = that_types.empty() ? nullptr : *that_types.begin();
        // Reject multiple types in the registers.
        if (this_types.size() > 1 || that_types.size() > 1 ||
            (this_type && that_type && this_type != that_type)) {
          reject(this_types, rejected_enums, CAST_THIS_POINTER);
          reject(that_types, rejected_enums, CAST_PARAMETER);
        }
        return;
      }
    } else if (signatures_match(method, STRINGBUILDER_APPEND_METHOD) ||
               signatures_match(method, ENUM_TOSTRING_METHOD) ||
               signatures_match(method, ENUM_NAME_METHOD)) {
      return;
    }

    // Check the type of arguments.
    const std::deque<DexType*>& args = proto->get_args()->get_type_list();
    always_assert(args.size() == insn->srcs_size() ||
                  args.size() == insn->srcs_size() - 1);
    size_t arg_id = 0;
    if (insn->srcs_size() == args.size() + 1) {
      // this pointer
      reject_if_inconsistent(env->get(insn->src(arg_id)),
                             insn->get_method()->get_class(), rejected_enums,
                             CAST_THIS_POINTER);
      arg_id++;
    }
    // Arguments
    auto it = args.begin();
    for (; arg_id < insn->srcs_size(); ++arg_id, ++it) {
      reject_if_inconsistent(env->get(insn->src(arg_id)), *it, rejected_enums,
                             CAST_PARAMETER);
    }
  }

  /**
   * If types of register is not consistent with required_type, remove these
   * types from candidate enum set.
   */
  void reject_if_inconsistent(const EnumTypes& types,
                              DexType* required_type,
                              ConcurrentSet<DexType*>* rejected_enums,
                              Reason reason = UNKNOWN) const {
    if (m_candidate_enums->count_unsafe(required_type)) {
      bool need_delete = false;
      for (auto possible_type : types.elements()) {
        if (!is_primitive(possible_type) && possible_type != required_type) {
          need_delete = true;
          reject(possible_type, rejected_enums, reason);
        }
      }
      if (need_delete) {
        reject(required_type, rejected_enums, reason);
      }
    } else {
      for (auto possible_type : types.elements()) {
        reject(possible_type, rejected_enums, reason);
      }
    }
  }

  void reject(std::unordered_set<DexType*> types,
              ConcurrentSet<DexType*>* rejected_enums,
              Reason reason = UNKNOWN) const {
    for (DexType* type : types) {
      reject(type, rejected_enums, reason);
    }
  }

  void reject(EnumTypes types,
              ConcurrentSet<DexType*>* rejected_enums,
              Reason reason = UNKNOWN) const {
    for (DexType* type : types.elements()) {
      reject(type, rejected_enums, reason);
    }
  }

  void reject(DexType* type,
              ConcurrentSet<DexType*>* rejected_enums,
              Reason reason = UNKNOWN) const {
    if (m_candidate_enums->count_unsafe(type)) {
      rejected_enums->insert(type);
      TRACE(ENUM, 9, "reject %s %d %s\n", SHOW(type), reason, SHOW(m_method));
    }
  }

  const DexMethodRef* ENUM_EQUALS_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z");
  const DexMethodRef* ENUM_COMPARETO_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.compareTo:(Ljava/lang/Enum;)I");
  const DexMethodRef* ENUM_TOSTRING_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.toString:()Ljava/lang/String;");
  const DexMethodRef* ENUM_NAME_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.name:()Ljava/lang/String;");
  const DexMethodRef* STRINGBUILDER_APPEND_METHOD = DexMethod::make_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/Object;)Ljava/lang/"
      "StringBuilder;");
  const DexString* VALUEOF_METHOD_STR = DexString::make_string("valueOf");
  const DexType* ENUM_TYPE = get_enum_type();
  const DexType* STRING_TYPE = get_string_type();

  const DexMethod* m_method;
  const ConcurrentSet<DexType*>* m_candidate_enums;
};
} // namespace

namespace optimize_enums {

/**
 * Analyze all the instructions that may involve object or type.
 */
void EnumFixpointIterator::analyze_instruction(IRInstruction* insn,
                                               EnumTypeEnvironment* env) const {
  const bool use_result =
      is_invoke(insn->opcode()) || insn->has_move_result_pseudo();
  if (use_result || insn->dests_size() > 0) {
    Register dest = use_result ? RESULT_REGISTER : insn->dest();

    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE:
      // Parameters are processed before we run FixpointIterator
      break;
    case OPCODE_MOVE_OBJECT:
      env->set(dest, env->get(insn->src(0)));
      break;
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_VIRTUAL:
      env->set(dest, EnumTypes(insn->get_method()->get_proto()->get_rtype()));
      break;
    case OPCODE_CONST_CLASS:
      env->set(dest, EnumTypes(get_class_type()));
      break;
    case OPCODE_CHECK_CAST:
      env->set(dest, EnumTypes(insn->get_type()));
      break;
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT:
      env->set(dest, env->get(RESULT_REGISTER));
      break;
    case OPCODE_SGET_OBJECT:
    case OPCODE_IGET_OBJECT: {
      DexType* type = insn->get_field()->get_type();
      if (!is_primitive(type)) {
        env->set(dest, EnumTypes(type));
      }
    } break;
    case OPCODE_AGET_OBJECT: {
      EnumTypes types;
      EnumTypes array_types = env->get(insn->src(0));
      for (const auto& array_type : array_types.elements()) {
        const auto type = get_array_type(array_type);
        if (type && !is_primitive(type)) {
          types.add(type);
        }
      }
      env->set(dest, types);
    } break;
    case OPCODE_NEW_ARRAY:
    case OPCODE_NEW_INSTANCE:
    case OPCODE_FILLED_NEW_ARRAY:
    case OPCODE_CONST_STRING: // We don't care about string object
    default:
      if (insn->has_type()) {
        env->set(dest, EnumTypes(insn->get_type()));
      } else {
        env->set(dest, EnumTypes());
      }
      // When we write a wide register v, the v+1 register is overrode.
      if (insn->dests_size() > 0 && insn->dest_is_wide()) {
        env->set(dest + 1, EnumTypes());
      }
      break;
    }
  }
}

/**
 * Generate environment with method parameter registers.
 */
EnumTypeEnvironment EnumFixpointIterator::gen_env(const DexMethod* method) {
  EnumTypeEnvironment env;
  const auto code = method->get_code()->get_param_instructions();
  const DexTypeList* args = method->get_proto()->get_args();
  const bool has_this_pointer = !is_static(method);
  size_t load_param_inst_size = 0;
  for (const auto& mie : InstructionIterable(code)) {
    ++load_param_inst_size;
  }
  always_assert(load_param_inst_size == args->size() + has_this_pointer);

  auto iterable = InstructionIterable(code);
  auto it = iterable.begin();
  if (has_this_pointer) { // Has this pointer
    env.set(it->insn->dest(), EnumTypes(method->get_class()));
    ++it;
  }
  for (DexType* type : args->get_type_list()) {
    env.set(it->insn->dest(), EnumTypes(type));
    ++it;
  }
  return env;
}

void reject_unsafe_enums(const std::vector<DexClass*>& classes,
                         ConcurrentSet<DexType*>* candidate_enums) {

  ConcurrentSet<DexType*> rejected_enums;

  const DexString* values_method = DexString::get_string("values");
  const DexString* valueof_method = DexString::get_string("valueOf");
  const DexType* string_type = get_string_type();

  // When do static analysis, simply skip javac-generated methods for enum
  // types : <clinit>, <init>, values(), valueOf(String)
  auto is_generated_enum_method = [&](const DexMethod* method) -> bool {
    auto& args = method->get_proto()->get_args()->get_type_list();
    return candidate_enums->count_unsafe(method->get_class()) &&
           !rejected_enums.count(method->get_class()) &&
           (is_clinit(method) || is_init(method) ||
            // values()
            (method->get_name() == values_method && args.size() == 0) ||
            // valueOf(String)
            (method->get_name() == valueof_method && args.size() == 1 &&
             args.front() == string_type));
  };

  walk::parallel::fields(classes, [&](DexField* field) {
    if (candidate_enums->count_unsafe(field->get_class())) {
      return;
    }
    auto type = field->get_type();
    if (is_array(type)) {
      type = get_array_type(type);
    }
    if (candidate_enums->count_unsafe(type) && !rejected_enums.count(type)) {
      if (!can_rename(field)) {
        rejected_enums.insert(type);
      }
    }
  });

  walk::parallel::methods(classes, [&](DexMethod* method) {
    // Skip generated enum methods
    if (is_generated_enum_method(method)) {
      return;
    }

    {
      std::vector<DexType*> types;
      method->get_proto()->gather_types(types);
      for (auto type : types) {
        if (is_array(type)) {
          type = get_array_type(type);
        }
        if (candidate_enums->count_unsafe(type) &&
            !rejected_enums.count(type)) {
          if (!can_rename(method)) {
            rejected_enums.insert(type);
          }
        }
      }
    }

    if (!need_analyze(method, *candidate_enums, rejected_enums)) {
      return;
    }

    EnumTypeEnvironment env = EnumFixpointIterator::gen_env(method);

    auto* code = method->get_code();
    code->build_cfg(/* editable */ false);
    EnumFixpointIterator engine(code->cfg());
    engine.run(env);

    EnumUpcastDetector detector(method, candidate_enums);
    detector.run(engine, code->cfg(), &rejected_enums);
    code->clear_cfg();
  });

  for (DexType* type : rejected_enums) {
    candidate_enums->erase(type);
  }
}
} // namespace optimize_enums
