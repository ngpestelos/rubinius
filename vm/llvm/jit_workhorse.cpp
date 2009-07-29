#ifdef ENABLE_LLVM

#include "llvm/jit_workhorse.hpp"
#include "instruments/profiler.hpp"
#include "call_frame.hpp"
#include "vmmethod.hpp"

#include "llvm/jit_visit.hpp"

/*
*/

/*
template <typename T>
static Value* constant(T obj, const Type* obj_type, BasicBlock* block) {
  return CastInst::Create(
      Instruction::IntToPtr,
      ConstantInt::get(Type::Int32Ty, (intptr_t)obj),
      obj_type, "cast_to_obj", block);
}
*/

namespace rubinius {
  LLVMWorkHorse::LLVMWorkHorse(LLVMState* ls)
    : ls_(ls)
  {
    llvm::Module* mod = ls->module();
    cf_type = mod->getTypeByName("struct.rubinius::CallFrame");
    vars_type = mod->getTypeByName("struct.rubinius::VariableScope");
    stack_vars_type = mod->getTypeByName("struct.rubinius::StackVariables");
    obj_type = ls->ptr_type("Object");
    obj_ary_type = PointerType::getUnqual(obj_type);
  }

  void LLVMWorkHorse::return_value(Value* ret, BasicBlock* cont) {
    if(ls_->include_profiling()) {
      Value* test = b().CreateLoad(ls_->profiling(), "profiling");
      BasicBlock* end_profiling = BasicBlock::Create("end_profiling", func);
      if(!cont) {
        cont = BasicBlock::Create("continue", func);
      }

      b().CreateCondBr(test, end_profiling, cont);

      b().SetInsertPoint(end_profiling);

      Signature sig(ls_, Type::VoidTy);
      sig << PointerType::getUnqual(Type::Int8Ty);

      Value* call_args[] = {
        method_entry_
      };

      sig.call("rbx_end_profiling", call_args, 1, "", b());

      b().CreateBr(cont);

      b().SetInsertPoint(cont);
    }

    b().CreateRet(ret);
  }

  Value* LLVMWorkHorse::get_field(Value* val, int which) {
    return b().CreateConstGEP2_32(val, 0, which);
  }

  void LLVMWorkHorse::initialize_call_frame(int stack_size) {
    Value* exec = b().CreateLoad(get_field(msg, 2), "msg.exec");
    Value* cm_gep = get_field(call_frame, offset::cf_cm);
    method = b().CreateBitCast(
        exec, cast<PointerType>(cm_gep->getType())->getElementType(), "cm");

    // previous
    b().CreateStore(prev, get_field(call_frame, offset::cf_previous));

    // msg
    b().CreateStore(msg, get_field(call_frame, offset::cf_msg));

    // cm
    b().CreateStore(method, cm_gep);

    // flags
    b().CreateStore(
        ConstantInt::get(Type::Int32Ty, 0),
        get_field(call_frame, offset::cf_flags));

    // ip
    b().CreateStore(
        ConstantInt::get(Type::Int32Ty, 0),
        get_field(call_frame, offset::cf_ip));

    // scope
    b().CreateStore(vars, get_field(call_frame, offset::cf_scope));

    if(ls_->include_profiling()) {
      method_entry_ = b().CreateAlloca(Type::Int8Ty,
          ConstantInt::get(Type::Int32Ty, sizeof(profiler::MethodEntry)),
          "method_entry");

      Value* test = b().CreateLoad(ls_->profiling(), "profiling");

      BasicBlock* setup_profiling = BasicBlock::Create("setup_profiling", func);
      BasicBlock* cont = BasicBlock::Create("continue", func);

      b().CreateCondBr(test, setup_profiling, cont);

      b().SetInsertPoint(setup_profiling);

      Signature sig(ls_, Type::VoidTy);
      sig << "VM";
      sig << PointerType::getUnqual(Type::Int8Ty);
      sig << "Dispatch";
      sig << "Arguments";
      sig << "CompiledMethod";

      Value* call_args[] = {
        vm,
        method_entry_,
        msg,
        args,
        method
      };

      sig.call("rbx_begin_profiling", call_args, 5, "", b());

      b().CreateBr(cont);

      b().SetInsertPoint(cont);
    }
  }

  void LLVMWorkHorse::initialize_block_frame(int stack_size) {
    Value* cm_gep = get_field(call_frame, offset::cf_cm);

    method = b().CreateLoad(get_field(block_env, offset::blockenv_method),
                            "env.method");

    // previous
    b().CreateStore(prev, get_field(call_frame, offset::cf_previous));

    // static_scope
    Value* ss = b().CreateLoad(get_field(block_inv, offset::blockinv_static_scope),
                               "invocation.static_scope");

    b().CreateStore(ss, get_field(call_frame, offset::cf_static_scope));

    // msg
    b().CreateStore(Constant::getNullValue(ls_->ptr_type("Dispatch")),
        get_field(call_frame, offset::cf_msg));

    // cm
    b().CreateStore(method, cm_gep);

    // flags
    Value* inv_flags = b().CreateLoad(get_field(block_inv, offset::blockinv_flags),
        "invocation.flags");

    int block_flags = CallFrame::cCustomStaticScope |
      CallFrame::cMultipleScopes;

    Value* flags = b().CreateOr(inv_flags,
        ConstantInt::get(Type::Int32Ty, block_flags), "flags");

    b().CreateStore(flags, get_field(call_frame, offset::cf_flags));

    // ip
    b().CreateStore(ConstantInt::get(Type::Int32Ty, 0),
        get_field(call_frame, offset::cf_ip));

    // scope
    b().CreateStore(vars, get_field(call_frame, offset::cf_scope));

    // top_scope
    top_scope = b().CreateLoad(
        get_field(block_env, offset::blockenv_top_scope),
        "env.top_scope");

    b().CreateStore(top_scope, get_field(call_frame, offset::cf_top_scope));

    if(ls_->include_profiling()) {
      method_entry_ = b().CreateAlloca(Type::Int8Ty,
          ConstantInt::get(Type::Int32Ty, sizeof(profiler::MethodEntry)),
          "method_entry");

      Value* test = b().CreateLoad(ls_->profiling(), "profiling");

      BasicBlock* setup_profiling = BasicBlock::Create("setup_profiling", func);
      BasicBlock* cont = BasicBlock::Create("continue", func);

      b().CreateCondBr(test, setup_profiling, cont);

      b().SetInsertPoint(setup_profiling);

      Signature sig(ls_, Type::VoidTy);
      sig << "VM";
      sig << PointerType::getUnqual(Type::Int8Ty);
      sig << "Dispatch";
      sig << "Arguments";
      sig << "CompiledMethod";

      Value* call_args[] = {
        vm,
        method_entry_,
        msg,
        args,
        method
      };

      sig.call("rbx_begin_profiling", call_args, 5, "", b());

      b().CreateBr(cont);

      b().SetInsertPoint(cont);
    }
  }

  void LLVMWorkHorse::nil_stack(int size, Value* nil) {
    if(size == 0) return;
    // Stack size 5 or less, do 5 stores in a row rather than
    // the loop.
    if(size <= 5) {
      for(int i = 0; i < size; i++) {
        b().CreateStore(nil, b().CreateConstGEP1_32(stk, i, "stack_pos"));
      }
      return;
    }

    Value* max = ConstantInt::get(Type::Int32Ty, size);
    Value* one = ConstantInt::get(Type::Int32Ty, 1);

    BasicBlock* top = BasicBlock::Create("stack_nil", func);
    BasicBlock* cont = BasicBlock::Create("bottom", func);

    Value* counter = b().CreateAlloca(Type::Int32Ty, 0, "counter_alloca");
    b().CreateStore(ConstantInt::get(Type::Int32Ty, 0), counter);

    b().CreateBr(top);

    b().SetInsertPoint(top);

    Value* cur = b().CreateLoad(counter, "counter");
    b().CreateStore(nil, b().CreateGEP(stk, cur, "stack_pos"));

    Value* added = b().CreateAdd(cur, one, "added");
    b().CreateStore(added, counter);

    Value* cmp = b().CreateICmpEQ(added, max, "loop_check");
    b().CreateCondBr(cmp, cont, top);

    b().SetInsertPoint(cont);
  }

  void LLVMWorkHorse::nil_locals(VMMethod* vmm) {
    Value* nil = constant(Qnil, obj_type);
    int size = vmm->number_of_locals;

    if(size == 0) return;
    // Stack size 5 or less, do 5 stores in a row rather than
    // the loop.
    if(size <= 5) {
      for(int i = 0; i < size; i++) {
        Value* idx[] = {
          ConstantInt::get(Type::Int32Ty, 0),
          ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
          ConstantInt::get(Type::Int32Ty, i)
        };

        Value* gep = b().CreateGEP(vars, idx, idx+3, "local_pos");
        b().CreateStore(nil, gep);
      }
      return;
    }

    Value* max = ConstantInt::get(Type::Int32Ty, size);
    Value* one = ConstantInt::get(Type::Int32Ty, 1);

    BasicBlock* top = BasicBlock::Create("locals_nil", func);
    BasicBlock* cont = BasicBlock::Create("bottom", func);

    Value* counter = b().CreateAlloca(Type::Int32Ty, 0, "counter_alloca");
    b().CreateStore(ConstantInt::get(Type::Int32Ty, 0), counter);

    b().CreateBr(top);

    b().SetInsertPoint(top);

    Value* cur = b().CreateLoad(counter, "counter");
    Value* idx[] = {
      ConstantInt::get(Type::Int32Ty, 0),
      ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
      cur
    };

    Value* gep = b().CreateGEP(vars, idx, idx+3, "local_pos");
    b().CreateStore(nil, gep);

    Value* added = b().CreateAdd(cur, one, "added");
    b().CreateStore(added, counter);

    Value* cmp = b().CreateICmpEQ(added, max, "loop_check");
    b().CreateCondBr(cmp, cont, top);

    b().SetInsertPoint(cont);
  }

  void LLVMWorkHorse::setup_scope(VMMethod* vmm) {
    Value* heap_null = ConstantExpr::getNullValue(PointerType::getUnqual(vars_type));
    Value* heap_pos = get_field(vars, offset::vars_on_heap);

    b().CreateStore(heap_null, heap_pos);

    Value* self = b().CreateLoad(get_field(args, offset::args_recv),
        "args.recv");
    b().CreateStore(self, get_field(vars, offset::vars_self));
    Value* mod = b().CreateLoad(get_field(msg, offset::msg_module),
        "msg.module");
    b().CreateStore(mod, get_field(vars, offset::vars_module));

    Value* blk = b().CreateLoad(get_field(args, offset::args_block),
        "args.block");
    b().CreateStore(blk, get_field(vars, offset::vars_block));

    b().CreateStore(Constant::getNullValue(ls_->ptr_type("VariableScope")),
        get_field(vars, offset::vars_parent));

    nil_locals(vmm);
  }

  void LLVMWorkHorse::setup_inline_scope(Value* self, Value* mod, VMMethod* vmm) {
    Value* heap_null = ConstantExpr::getNullValue(PointerType::getUnqual(vars_type));
    Value* heap_pos = get_field(vars, offset::vars_on_heap);
    b().CreateStore(heap_null, heap_pos);

    b().CreateStore(self, get_field(vars, offset::vars_self));
    b().CreateStore(mod, get_field(vars, offset::vars_module));

    Value* blk = constant(Qnil, obj_type);
    b().CreateStore(blk, get_field(vars, offset::vars_block));

    b().CreateStore(Constant::getNullValue(ls_->ptr_type("VariableScope")),
        get_field(vars, offset::vars_parent));

    nil_locals(vmm);
  }

  void LLVMWorkHorse::setup_block_scope(VMMethod* vmm) {
    b().CreateStore(ConstantExpr::getNullValue(PointerType::getUnqual(vars_type)),
        get_field(vars, offset::vars_on_heap));
    Value* self = b().CreateLoad(
        get_field(block_inv, offset::blockinv_self),
        "invocation.self");

    b().CreateStore(self, get_field(vars, offset::vars_self));

    Value* mod = b().CreateLoad(get_field(top_scope, offset::varscope_module),
        "top_scope.module");
    b().CreateStore(mod, get_field(vars, offset::vars_module));

    Value* blk = b().CreateLoad(get_field(top_scope, offset::varscope_block),
        "args.block");
    b().CreateStore(blk, get_field(vars, offset::vars_block));


    // We don't use top_scope here because of nested blocks. Parent MUST be
    // the scope the block was created in, not the top scope for depth
    // variables to work.
    Value* be_scope = b().CreateLoad(
        get_field(block_env, offset::blockenv_scope),
        "env.scope");

    b().CreateStore(be_scope, get_field(vars, offset::vars_parent));

    nil_locals(vmm);
  }

  void LLVMWorkHorse::check_arity(VMMethod* vmm) {
    Value* vm_obj = vm;
    Value* dis_obj = msg;
    Value* arg_obj = args;

    Value* total_offset = b().CreateConstGEP2_32(arg_obj, 0,
        offset::args_total, "total_pos");
    Value* total = b().CreateLoad(total_offset, "arg.total");

    // For others to use.
    arg_total = total;

    BasicBlock* arg_error = BasicBlock::Create("arg_error", func);
    BasicBlock* cont = BasicBlock::Create("import_args", func);

    // Check arguments
    //
    // if there is a splat..
    if(vmm->splat_position >= 0) {
      if(vmm->required_args > 0) {
        // Make sure we got at least the required args
        Value* cmp = b().CreateICmpSLT(total,
            ConstantInt::get(Type::Int32Ty, vmm->required_args), "arg_cmp");
        b().CreateCondBr(cmp, arg_error, cont);
      } else {
        // Only splat or optionals, no handling!
        b().CreateBr(cont);
      }

      // No splat, a precise number of args
    } else if(vmm->required_args == vmm->total_args) {
      // Make sure we got the exact number of arguments
      Value* cmp = b().CreateICmpNE(total,
          ConstantInt::get(Type::Int32Ty, vmm->required_args), "arg_cmp");
      b().CreateCondBr(cmp, arg_error, cont);

      // No splat, with optionals
    } else {
      Value* c1 = b().CreateICmpSLT(total,
          ConstantInt::get(Type::Int32Ty, vmm->required_args), "arg_cmp");
      Value* c2 = b().CreateICmpSGT(total,
          ConstantInt::get(Type::Int32Ty, vmm->total_args), "arg_cmp");

      Value* cmp = b().CreateOr(c1, c2, "arg_combine");
      b().CreateCondBr(cmp, arg_error, cont);
    }

    b().SetInsertPoint(arg_error);

    // Call our arg_error helper
    Signature sig(ls_, "Object");

    sig << "VM";
    sig << "CallFrame";
    sig << "Dispatch";
    sig << "Arguments";
    sig << Type::Int32Ty;

    Value* call_args[] = {
      vm_obj,
      prev,
      dis_obj,
      arg_obj,
      ConstantInt::get(Type::Int32Ty, vmm->required_args)
    };

    Value* val = sig.call("rbx_arg_error", call_args, 5, "ret", b());
    return_value(val);

    // Switch to using continuation
    b().SetInsertPoint(cont);
  }

  void LLVMWorkHorse::import_args(VMMethod* vmm) {
    Value* vm_obj = vm;
    Value* arg_obj = args;

    setup_scope(vmm);

    // Import the arguments
    Value* offset = b().CreateConstGEP2_32(args, 0, offset::args_ary, "arg_ary_pos");

    Value* arg_ary = b().CreateLoad(offset, "arg_ary");

    // If there are a precise number of args, easy.
    if(vmm->required_args == vmm->total_args) {
      for(int i = 0; i < vmm->required_args; i++) {
        Value* int_pos = ConstantInt::get(Type::Int32Ty, i);

        Value* arg_val_offset = b().CreateConstGEP1_32(arg_ary, i, "arg_val_offset");

        Value* arg_val = b().CreateLoad(arg_val_offset, "arg_val");

        Value* idx2[] = {
          ConstantInt::get(Type::Int32Ty, 0),
          ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
          int_pos
        };

        Value* pos = b().CreateGEP(vars, idx2, idx2+3, "var_pos");

        b().CreateStore(arg_val, pos);
      }

      // Otherwise, we must loop in the generate code because we don't know
      // how many they've actually passed in.
    } else {
      Value* loop_i = b().CreateAlloca(Type::Int32Ty, 0, "loop_i");

      BasicBlock* top = BasicBlock::Create("arg_loop_top", func);
      BasicBlock* body = BasicBlock::Create("arg_loop_body", func);
      BasicBlock* after = BasicBlock::Create("arg_loop_cont", func);

      b().CreateStore(ConstantInt::get(Type::Int32Ty, 0), loop_i);
      b().CreateBr(top);

      b().SetInsertPoint(top);

      // now at the top of block, check if we should continue...
      Value* loop_val = b().CreateLoad(loop_i, "loop_val");
      Value* cmp = b().CreateICmpSLT(loop_val, arg_total, "loop_test");

      b().CreateCondBr(cmp, body, after);

      // Now, the body
      b().SetInsertPoint(body);

      Value* arg_val_offset =
        b().CreateGEP(arg_ary, loop_val, "arg_val_offset");

      Value* arg_val = b().CreateLoad(arg_val_offset, "arg_val");

      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
        loop_val
      };

      Value* pos = b().CreateGEP(vars, idx2, idx2+3, "var_pos");

      b().CreateStore(arg_val, pos);

      Value* plus_one = b().CreateAdd(loop_val,
          ConstantInt::get(Type::Int32Ty, 1), "add");
      b().CreateStore(plus_one, loop_i);

      b().CreateBr(top);

      b().SetInsertPoint(after);
    }

    // Setup the splat.
    if(vmm->splat_position >= 0) {
      Signature sig(ls_, "Object");
      sig << "VM";
      sig << "Arguments";
      sig << Type::Int32Ty;

      Value* call_args[] = {
        vm_obj,
        arg_obj,
        ConstantInt::get(Type::Int32Ty, vmm->total_args)
      };

      Function* func = sig.function("rbx_construct_splat");
      func->setOnlyReadsMemory(true);
      func->setDoesNotThrow(true);

      CallInst* splat_val = sig.call("rbx_construct_splat", call_args, 3, "splat_val", b());

      splat_val->setOnlyReadsMemory(true);
      splat_val->setDoesNotThrow(true);

      Value* idx3[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
        ConstantInt::get(Type::Int32Ty, vmm->splat_position)
      };

      Value* pos = b().CreateGEP(vars, idx3, idx3+3, "splat_pos");
      b().CreateStore(splat_val, pos);
    }
  }

  void LLVMWorkHorse::setup_block(VMMethod* vmm) {
    Signature sig(ls_, "Object");
    sig << "VM";
    sig << "CallFrame";
    sig << "BlockEnvironment";
    sig << "Arguments";
    sig << "BlockInvocation";

    func = sig.function("");

    Function::arg_iterator ai = func->arg_begin();
    vm =   ai++; vm->setName("state");
    prev = ai++; prev->setName("previous");
    block_env = ai++; block_env->setName("env");
    args = ai++; args->setName("args");
    block_inv = ai++; block_inv->setName("invocation");

    BasicBlock* block = BasicBlock::Create("entry", func);
    b().SetInsertPoint(block);

    valid_flag = b().CreateAlloca(Type::Int1Ty, 0, "valid_flag");

    Value* cfstk = b().CreateAlloca(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(CallFrame) / sizeof(Object*)) + vmm->stack_size),
        "cfstk");

    call_frame = b().CreateBitCast(
        cfstk,
        PointerType::getUnqual(cf_type), "call_frame");

    stk = b().CreateConstGEP1_32(cfstk, sizeof(CallFrame) / sizeof(Object*), "stack");

    Value* var_mem = b().CreateAlloca(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(StackVariables) / sizeof(Object*)) + vmm->number_of_locals),
        "var_mem");

    vars = b().CreateBitCast(
        var_mem,
        PointerType::getUnqual(stack_vars_type), "vars");

    initialize_block_frame(vmm->stack_size);

    nil_stack(vmm->stack_size, constant(Qnil, obj_type));

    setup_block_scope(vmm);

    BasicBlock* body = BasicBlock::Create("block_body", func);
    b().CreateBr(body);

    b().SetInsertPoint(body);
  }

  void LLVMWorkHorse::setup(VMMethod* vmm) {
    Signature sig(ls_, "Object");
    sig << "VM";
    sig << "CallFrame";
    sig << "Dispatch";
    sig << "Arguments";

    func = sig.function("");

    Function::arg_iterator ai = func->arg_begin();
    vm =   ai++; vm->setName("state");
    prev = ai++; prev->setName("previous");
    msg =  ai++; msg->setName("msg");
    args = ai++; args->setName("args");

    BasicBlock* block = BasicBlock::Create("entry", func);
    builder_.SetInsertPoint(block);

    valid_flag = b().CreateAlloca(Type::Int1Ty, 0, "valid_flag");

    Value* cfstk = b().CreateAlloca(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(CallFrame) / sizeof(Object*)) + vmm->stack_size),
        "cfstk");

    Value* var_mem = b().CreateAlloca(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(StackVariables) / sizeof(Object*)) + vmm->number_of_locals),
        "var_mem");

    check_arity(vmm);

    call_frame = b().CreateBitCast(
        cfstk,
        PointerType::getUnqual(cf_type), "call_frame");

    stk = b().CreateConstGEP1_32(cfstk, sizeof(CallFrame) / sizeof(Object*), "stack");

    vars = b().CreateBitCast(
        var_mem,
        PointerType::getUnqual(stack_vars_type), "vars");

    initialize_call_frame(vmm->stack_size);

    nil_stack(vmm->stack_size, constant(Qnil, obj_type));

    import_args(vmm);

    BasicBlock* body = BasicBlock::Create("method_body", func);

    b().CreateBr(body);
    b().SetInsertPoint(body);
  }

  BasicBlock* LLVMWorkHorse::setup_inline(VMMethod* vmm, Function* current,
      Value* vm_i, Value* previous,
      Value* self, Value* mod, std::vector<Value*>& stack_args)
  {
    func = current;
    vm = vm_i;
    prev = previous;
    args = ConstantExpr::getNullValue(ls_->ptr_type("Arguments"));

    BasicBlock* entry = BasicBlock::Create("inline_entry", func);
    b().SetInsertPoint(entry);

    BasicBlock* alloca_block = &current->getEntryBlock();

    Value* cfstk = new AllocaInst(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(CallFrame) / sizeof(Object*)) + vmm->stack_size),
        "cfstk", alloca_block->getTerminator());

    call_frame = b().CreateBitCast(
        cfstk,
        PointerType::getUnqual(cf_type), "call_frame");

    stk = b().CreateConstGEP1_32(cfstk, sizeof(CallFrame) / sizeof(Object*), "stack");

    Value* var_mem = new AllocaInst(obj_type,
        ConstantInt::get(Type::Int32Ty,
          (sizeof(StackVariables) / sizeof(Object*)) + vmm->number_of_locals),
        "var_mem", alloca_block->getTerminator());

    vars = b().CreateBitCast(
        var_mem,
        PointerType::getUnqual(stack_vars_type), "vars");

    //  Setup the CallFrame
    //
    // previous
    b().CreateStore(prev, get_field(call_frame, offset::cf_previous));

    // msg
    b().CreateStore(ConstantExpr::getNullValue(ls_->ptr_type("Dispatch")),
        get_field(call_frame, offset::cf_msg));

    // cm
    Value* obj_addr = constant(vmm->original.object_address(),
        PointerType::getUnqual(ls_->ptr_type("CompiledMethod")));

    method = b().CreateLoad(obj_addr, "cm");
    Value* cm_gep = get_field(call_frame, offset::cf_cm);
    b().CreateStore(method, cm_gep);

    // flags
    b().CreateStore(ConstantInt::get(Type::Int32Ty, CallFrame::cInlineFrame),
        get_field(call_frame, offset::cf_flags));

    // ip
    b().CreateStore(ConstantInt::get(Type::Int32Ty, 0),
        get_field(call_frame, offset::cf_ip));

    // scope
    b().CreateStore(vars, get_field(call_frame, offset::cf_scope));

    nil_stack(vmm->stack_size, constant(Qnil, obj_type));

    setup_inline_scope(self, mod, vmm);

    // We know the right arguments are present, so we just need to put them
    // in the right place.
    //
    // We don't support splat in an inlined method!
    assert(vmm->splat_position < 0);

    assert(stack_args.size() <= (size_t)vmm->total_args);

    for(size_t i = 0; i < stack_args.size(); i++) {
      Value* int_pos = ConstantInt::get(Type::Int32Ty, i);

      Value* idx2[] = {
        ConstantInt::get(Type::Int32Ty, 0),
        ConstantInt::get(Type::Int32Ty, offset::vars_tuple),
        int_pos
      };

      Value* pos = b().CreateGEP(vars, idx2, idx2+3, "local_pos");

      b().CreateStore(stack_args[i], pos);
    }

    BasicBlock* body = BasicBlock::Create("method_body", func);
    b().CreateBr(body);
    b().SetInsertPoint(body);

    return entry;
  }

  class BlockFinder : public VisitInstructions<BlockFinder> {
    BlockMap& map_;
    Function* function_;
    opcode current_ip_;
    int force_break_;
    bool creates_blocks_;
    int number_of_sends_;
    bool loops_;
    int sp_;
    JITBasicBlock* current_block_;

  public:

    BlockFinder(BlockMap& map, Function* func, BasicBlock* start)
      : map_(map)
      , function_(func)
      , current_ip_(0)
      , force_break_(false)
      , creates_blocks_(false)
      , number_of_sends_(0)
      , loops_(false)
      , sp_(-1)
    {
      JITBasicBlock& jbb = map_[0];
      jbb.reachable = true;
      jbb.block = start;

      current_block_ = &jbb;
    }

    bool creates_blocks() {
      return creates_blocks_;
    }

    int number_of_sends() {
      return number_of_sends_;
    }

    bool loops_p() {
      return loops_;
    }

    void at_ip(int ip) {
      current_ip_ = ip;

      // If this is a new block, reset sp here
      /*
      BlockMap::iterator i = map_.find(ip);
      if(i != map_.end()) {
        sp_ = i->second.sp;
      }
      */
    }

    const static int cUnknown = -10;
    const static bool cDebugStack = false;

#include "gen/inst_stack.hpp"

    bool before(opcode op, opcode arg1=0, opcode arg2=0) {
      BlockMap::iterator i = map_.find(current_ip_);
      if(i != map_.end()) {
        if(i->second.sp == cUnknown) {
          if(cDebugStack) {
            std::cout << current_ip_ << ": " << sp_ << " (inherit)\n";
          }
          i->second.sp = sp_;
        } else {
          sp_ = i->second.sp;
          if(cDebugStack) {
            std::cout << current_ip_ << ": " << sp_ << " (reset)\n";
          }
        }

        current_block_ = &i->second;
      } else {
        if(force_break_) {
          if(cDebugStack) {
            std::cout << current_ip_ << ": dead\n";
          }
          return false;
        }

        if(cDebugStack) {
          std::cout << current_ip_ << ": " << sp_ << "\n";
        }
      }

      // Update current_block everytime. When current_block changes,
      // previous current blocks will thereby contain their real end_ip
      current_block_->end_ip = current_ip_;

      force_break_ = false;
      if(sp_ != cUnknown) {
        sp_ += stack_difference(op, arg1, arg2);
        assert(sp_ >= -1);
      }

      return true;
    }

    void break_at(opcode ip) {
      BlockMap::iterator i = map_.find(ip);
      if(i == map_.end()) {
        std::ostringstream ss;
        ss << "ip" << ip;
        JITBasicBlock& jbb = map_[ip];
        jbb.block = BasicBlock::Create(ss.str().c_str(), function_);
        jbb.start_ip = ip;
        jbb.sp = sp_;

        if(ip < current_ip_) {
          jbb.end_ip = current_ip_;
        }

        if(cDebugStack) {
          std::cout << "patch " << ip << ": " << jbb.sp << "\n";
        }
      } else {
        assert(i->second.sp == sp_);
      }
    }

    void next_ip_too() {
      force_break_ = true;
    }

    void visit_goto(opcode which) {
      if(current_ip_ < which) loops_ = true;

      break_at(which);
      next_ip_too();
    }

    void visit_goto_if_true(opcode which) {
      if(current_ip_ < which) loops_ = true;

      break_at(which);
      break_at(current_ip_ + 2);
    }

    void visit_goto_if_false(opcode which) {
      if(current_ip_ < which) loops_ = true;

      break_at(which);
      break_at(current_ip_ + 2);
    }

    void visit_goto_if_defined(opcode which) {
      if(current_ip_ < which) loops_ = true;

      break_at(which);
      break_at(current_ip_ + 2);
    }

    void visit_setup_unwind(opcode which, opcode type) {
      if(current_ip_ < which) loops_ = true;

      break_at(which);
    }

    void visit_ret() {
      next_ip_too();
    }

    void visit_raise_return() {
      next_ip_too();
    }

    void visit_raise_break() {
      next_ip_too();
    }

    void visit_ensure_return() {
      next_ip_too();
    }

    void visit_reraise() {
      next_ip_too();
    }

    void visit_raise_exc() {
      next_ip_too();
    }

    void visit_create_block(opcode which) {
      creates_blocks_ = true;
    }

    void visit_send_stack(opcode which, opcode args) {
      number_of_sends_++;
    }

    void visit_send_method(opcode which) {
      number_of_sends_++;
    }

    void visit_send_stack_with_block(opcode which, opcode args) {
      number_of_sends_++;
    }

    void visit_send_stack_with_splat(opcode which, opcode args) {
      number_of_sends_++;
    }

    void visit_send_super_stack_with_block(opcode which, opcode args) {
      number_of_sends_++;
    }

    void visit_send_super_stack_with_splat(opcode which, opcode args) {
      number_of_sends_++;
    }
  };

  bool LLVMWorkHorse::generate_body(JITMethodInfo& info) {
    JITVisit visitor(ls_, info, ls_->module(), func,
        b().GetInsertBlock(), stk, call_frame,
        method_entry_, args,
        vars, info.is_block, info.inline_return);

    if(info.inline_policy) {
      visitor.set_policy(info.inline_policy);
    } else {
      visitor.init_policy();
    }

    visitor.set_called_args(info.called_args);

    visitor.set_valid_flag(valid_flag);

    // Pass 1, detect BasicBlock boundaries
    BlockFinder finder(visitor.block_map(), func, b().GetInsertBlock());
    finder.drive(info.vmm);

    // DISABLED: This still has problems.
    // visitor.set_creates_blocks(finder.creates_blocks());

    if(!info.inline_return &&
          (finder.number_of_sends() > 0 || finder.loops_p())) {
      // Check for interrupts at the top
      visitor.visit_check_interrupts();
    }

    // std::cout << info.vmm << " start: " << info.vmm->total << "\n";

    // Fix up the JITBasicBlock's so that the ranges don't overlap
    // (overlap is caused by a backward branch)
    JITBasicBlock* prev = 0;
    BlockMap& bm = visitor.block_map();
    for(BlockMap::iterator i = bm.begin();
          i != bm.end();
          i++) {
      JITBasicBlock& jbb = i->second;
      if(prev && prev->end_ip >= jbb.start_ip) {
        /*
        std::cout << info.vmm << " overlap: "
          << prev->start_ip << "-" << prev->end_ip << " "
          << jbb.start_ip << "-" << jbb.end_ip
          << "\n";
        */
        prev->end_ip = jbb.start_ip - 1;

        /*
        std::cout << info.vmm << " split region: " << prev->end_ip << "\n";
        */
      }

      prev = &jbb;
    }

    // Pass 2, compile!
    try {
      // Drive visitor for only blocks, as they contain all live regions
      for(BlockMap::iterator i = bm.begin();
          i != bm.end();
          i++) {
        JITBasicBlock& jbb = i->second;
        /*
        std::cout << info.vmm
          << " Driving: " << jbb.start_ip << "-" << jbb.end_ip << "\n";
        */
        visitor.drive(info.vmm->opcodes, jbb.end_ip+1, jbb.start_ip);
      }
    } catch(JITVisit::Unsupported &e) {
      return false;
    }

    info.return_value = visitor.return_value();
    info.fin_block = visitor.current_block();
    return true;
  }
}

#endif
