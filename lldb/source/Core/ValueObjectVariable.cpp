//===-- ValueObjectVariable.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/ValueObjectVariable.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/Value.h"
#include "lldb/Expression/DWARFExpression.h"
#include "lldb/Symbol/Declaration.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolContextScope.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/RegisterValue.h"
#include "lldb/Utility/Scalar.h"
#include "lldb/Utility/Status.h"
#include "lldb/lldb-private-enumerations.h"
#include "lldb/lldb-types.h"

#include "llvm/ADT/StringRef.h"

#include <assert.h>
#include <memory>

namespace lldb_private {
class ExecutionContextScope;
}
namespace lldb_private {
class StackFrame;
}
namespace lldb_private {
struct RegisterInfo;
}
using namespace lldb_private;

lldb::ValueObjectSP
ValueObjectVariable::Create(ExecutionContextScope *exe_scope,
                            const lldb::VariableSP &var_sp) {
  return (new ValueObjectVariable(exe_scope, var_sp))->GetSP();
}

ValueObjectVariable::ValueObjectVariable(ExecutionContextScope *exe_scope,
                                         const lldb::VariableSP &var_sp)
    : ValueObject(exe_scope), m_variable_sp(var_sp) {
  // Do not attempt to construct one of these objects with no variable!
  assert(m_variable_sp.get() != nullptr);
  m_name = var_sp->GetName();
}

ValueObjectVariable::~ValueObjectVariable() {}

CompilerType ValueObjectVariable::GetCompilerTypeImpl() {
  Type *var_type = m_variable_sp->GetType();
  if (var_type)
    return var_type->GetForwardCompilerType();
  return CompilerType();
}

ConstString ValueObjectVariable::GetTypeName() {
  Type *var_type = m_variable_sp->GetType();
  if (var_type)
    return var_type->GetName();
  return ConstString();
}

ConstString ValueObjectVariable::GetDisplayTypeName() {
  Type *var_type = m_variable_sp->GetType();
  if (var_type) {
      const SymbolContext *sc = nullptr;
      if (GetFrameSP())
        sc = &GetFrameSP()->GetSymbolContext(lldb::eSymbolContextFunction);
      return var_type->GetForwardCompilerType().GetDisplayTypeName(sc);
  }
  return ConstString();
}

ConstString ValueObjectVariable::GetQualifiedTypeName() {
  Type *var_type = m_variable_sp->GetType();
  if (var_type)
    return var_type->GetQualifiedName();
  return ConstString();
}

size_t ValueObjectVariable::CalculateNumChildren(uint32_t max) {
  CompilerType type(GetCompilerType());

  if (!type.IsValid())
    return 0;

  ExecutionContext exe_ctx(GetExecutionContextRef());
  const bool omit_empty_base_classes = true;
  auto child_count = type.GetNumChildren(omit_empty_base_classes, &exe_ctx);
  return child_count <= max ? child_count : max;
}

uint64_t ValueObjectVariable::GetByteSize() {
  ExecutionContext exe_ctx(GetExecutionContextRef());

  CompilerType type(GetCompilerType());

  if (!type.IsValid())
    return 0;

  return type.GetByteSize(exe_ctx.GetBestExecutionContextScope()).getValueOr(0);
}

lldb::ValueType ValueObjectVariable::GetValueType() const {
  if (m_variable_sp)
    return m_variable_sp->GetScope();
  return lldb::eValueTypeInvalid;
}

bool ValueObjectVariable::UpdateValue() {
  SetValueIsValid(false);
  m_error.Clear();

  Variable *variable = m_variable_sp.get();
  // Check if the type has size 0. If so, there is nothing to update,
  // unless the type is C++ where even empty structs have a non-zero
  // size.
  CompilerType var_type(GetCompilerTypeImpl());
  if (var_type.IsValid() && var_type.GetMinimumLanguage() == lldb::eLanguageTypeSwift) {
    ExecutionContext exe_ctx(GetExecutionContextRef());
    llvm::Optional<uint64_t> size =
      var_type.GetByteSize(exe_ctx.GetBestExecutionContextScope());
    if (size && *size == 0) {
      m_value.SetCompilerType(var_type);
      return m_error.Success();
    }
  }

  DWARFExpression &expr = variable->LocationExpression();

  if (variable->GetLocationIsConstantValueData()) {
    // expr doesn't contain DWARF bytes, it contains the constant variable
    // value bytes themselves...
    if (expr.GetExpressionData(m_data)) {
      if (m_data.GetDataStart() && m_data.GetByteSize())
        m_value.SetBytes(m_data.GetDataStart(), m_data.GetByteSize());
      m_value.SetContext(Value::eContextTypeVariable, variable);
    } else
      m_error.SetErrorString("empty constant data");
    // constant bytes can't be edited - sorry
    m_resolved_value.SetContext(Value::eContextTypeInvalid, nullptr);
    SetAddressTypeOfChildren(eAddressTypeInvalid);
  } else {
    lldb::addr_t loclist_base_load_addr = LLDB_INVALID_ADDRESS;
    ExecutionContext exe_ctx(GetExecutionContextRef());

    Target *target = exe_ctx.GetTargetPtr();
    if (target) {
      m_data.SetByteOrder(target->GetArchitecture().GetByteOrder());
      m_data.SetAddressByteSize(target->GetArchitecture().GetAddressByteSize());
    }

    if (expr.IsLocationList()) {
      SymbolContext sc;
      variable->CalculateSymbolContext(&sc);
      if (sc.function)
        loclist_base_load_addr =
            sc.function->GetAddressRange().GetBaseAddress().GetLoadAddress(
                target);
    }
    Value old_value(m_value);
    if (expr.Evaluate(&exe_ctx, nullptr, loclist_base_load_addr, nullptr,
                      nullptr, m_value, &m_error)) {
      m_resolved_value = m_value;
      m_value.SetContext(Value::eContextTypeVariable, variable);

      CompilerType compiler_type = GetCompilerType();
      if (compiler_type.IsValid())
        m_value.SetCompilerType(compiler_type);

      Value::ValueType value_type = m_value.GetValueType();

      Process *process = exe_ctx.GetProcessPtr();
      const bool process_is_alive = process && process->IsAlive();

      // BEGIN Swift
      if (variable->GetType() && variable->GetType()->IsSwiftFixedValueBuffer())
        if (auto process_sp = GetProcessSP())
          if (auto runtime = process_sp->GetLanguageRuntime(
                  compiler_type.GetMinimumLanguage())) {
            if (!runtime->IsStoredInlineInBuffer(compiler_type)) {
              lldb::addr_t addr =
                  m_value.GetScalar().ULongLong(LLDB_INVALID_ADDRESS);
              if (addr != LLDB_INVALID_ADDRESS) {
                Target &target = process_sp->GetTarget();
                size_t ptr_size = process_sp->GetAddressByteSize();
                lldb::addr_t deref_addr;
                target.ReadMemory(addr, false, &deref_addr, ptr_size, m_error);
                m_value.GetScalar() = deref_addr;
              }
            }
          }
      // END Swift

      switch (value_type) {
      case Value::eValueTypeVector:
      // fall through
      case Value::eValueTypeScalar:
        // The variable value is in the Scalar value inside the m_value. We can
        // point our m_data right to it.
        m_error =
            m_value.GetValueAsData(&exe_ctx, m_data, GetModule().get());
        break;

      case Value::eValueTypeFileAddress:
      case Value::eValueTypeLoadAddress:
      case Value::eValueTypeHostAddress:
        // The DWARF expression result was an address in the inferior process.
        // If this variable is an aggregate type, we just need the address as
        // the main value as all child variable objects will rely upon this
        // location and add an offset and then read their own values as needed.
        // If this variable is a simple type, we read all data for it into
        // m_data. Make sure this type has a value before we try and read it

        // If we have a file address, convert it to a load address if we can.
        if (value_type == Value::eValueTypeFileAddress && process_is_alive)
          m_value.ConvertToLoadAddress(GetModule().get(), target);

        if (!CanProvideValue()) {
          // this value object represents an aggregate type whose children have
          // values, but this object does not. So we say we are changed if our
          // location has changed.
          SetValueDidChange(value_type != old_value.GetValueType() ||
                            m_value.GetScalar() != old_value.GetScalar());
        } else {
          // Copy the Value and set the context to use our Variable so it can
          // extract read its value into m_data appropriately
          Value value(m_value);
          value.SetContext(Value::eContextTypeVariable, variable);
          m_error =
              value.GetValueAsData(&exe_ctx, m_data, GetModule().get());

          SetValueDidChange(value_type != old_value.GetValueType() ||
                            m_value.GetScalar() != old_value.GetScalar());
        }
        break;
      }

      SetValueIsValid(m_error.Success());
    } else {
      // could not find location, won't allow editing
      m_resolved_value.SetContext(Value::eContextTypeInvalid, nullptr);
    }
  }
  return m_error.Success();
}

bool ValueObjectVariable::IsInScope() {
  const ExecutionContextRef &exe_ctx_ref = GetExecutionContextRef();
  if (exe_ctx_ref.HasFrameRef()) {
    ExecutionContext exe_ctx(exe_ctx_ref);
    StackFrame *frame = exe_ctx.GetFramePtr();
    if (frame) {
      return m_variable_sp->IsInScope(frame);
    } else {
      // This ValueObject had a frame at one time, but now we can't locate it,
      // so return false since we probably aren't in scope.
      return false;
    }
  }
  // We have a variable that wasn't tied to a frame, which means it is a global
  // and is always in scope.
  return true;
}

lldb::ModuleSP ValueObjectVariable::GetModule() {
  if (m_variable_sp) {
    SymbolContextScope *sc_scope = m_variable_sp->GetSymbolContextScope();
    if (sc_scope) {
      return sc_scope->CalculateSymbolContextModule();
    }
  }
  return lldb::ModuleSP();
}

SymbolContextScope *ValueObjectVariable::GetSymbolContextScope() {
  if (m_variable_sp)
    return m_variable_sp->GetSymbolContextScope();
  return nullptr;
}

bool ValueObjectVariable::GetDeclaration(Declaration &decl) {
  if (m_variable_sp) {
    decl = m_variable_sp->GetDeclaration();
    return true;
  }
  return false;
}

const char *ValueObjectVariable::GetLocationAsCString() {
  if (m_resolved_value.GetContextType() == Value::eContextTypeRegisterInfo)
    return GetLocationAsCStringImpl(m_resolved_value, m_data);
  else
    return ValueObject::GetLocationAsCString();
}

bool ValueObjectVariable::SetValueFromCString(const char *value_str,
                                              Status &error) {
  if (!UpdateValueIfNeeded()) {
    error.SetErrorString("unable to update value before writing");
    return false;
  }

  if (m_resolved_value.GetContextType() == Value::eContextTypeRegisterInfo) {
    RegisterInfo *reg_info = m_resolved_value.GetRegisterInfo();
    ExecutionContext exe_ctx(GetExecutionContextRef());
    RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
    RegisterValue reg_value;
    if (!reg_info || !reg_ctx) {
      error.SetErrorString("unable to retrieve register info");
      return false;
    }
    error = reg_value.SetValueFromString(reg_info, llvm::StringRef(value_str));
    if (error.Fail())
      return false;
    if (reg_ctx->WriteRegister(reg_info, reg_value)) {
      SetNeedsUpdate();
      return true;
    } else {
      error.SetErrorString("unable to write back to register");
      return false;
    }
  } else
    return ValueObject::SetValueFromCString(value_str, error);
}

bool ValueObjectVariable::SetData(DataExtractor &data, Status &error) {
  if (!UpdateValueIfNeeded()) {
    error.SetErrorString("unable to update value before writing");
    return false;
  }

  if (m_resolved_value.GetContextType() == Value::eContextTypeRegisterInfo) {
    RegisterInfo *reg_info = m_resolved_value.GetRegisterInfo();
    ExecutionContext exe_ctx(GetExecutionContextRef());
    RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
    RegisterValue reg_value;
    if (!reg_info || !reg_ctx) {
      error.SetErrorString("unable to retrieve register info");
      return false;
    }
    error = reg_value.SetValueFromData(reg_info, data, 0, true);
    if (error.Fail())
      return false;
    if (reg_ctx->WriteRegister(reg_info, reg_value)) {
      SetNeedsUpdate();
      return true;
    } else {
      error.SetErrorString("unable to write back to register");
      return false;
    }
  } else
    return ValueObject::SetData(data, error);
}
