/*
* The MIT License (MIT)
*
* Copyright (c) Microsoft Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
*/

#ifndef ABSINT_H
#define ABSINT_H

#include <Python.h>
#include <vector>
#include <unordered_map>

#include "absvalue.h"
#include "taggedptr.h"
#include "intrins.h"
#include "cowvector.h"
#include "ipycomp.h"

using namespace std;

struct AbstractLocalInfo;
struct AbsIntBlockInfo;
class InterpreterState;

#define FIRST_USER_FUNCTION_TOKEN   0x00100000

// Tracks the state of a local variable at each location in the function.
// Each local has a known type associated with it as well as whether or not
// the value is potentially undefined.  When a variable is definitely assigned
// IsMaybeUndefined is false.
//
// Initially all locals start out as being marked as IsMaybeUndefined and a special
// typeof Undefined.  The special type is really just for convenience to avoid
// having null types.  Merging with the undefined type will produce the other type.
// Assigning to a variable will cause the undefined marker to be removed, and the
// new type to be specified.
//
// When we merge locals if the undefined flag is specified from either side we will
// propagate it to the new state.  This could result in:
//
// State 1: Type != Undefined, IsMaybeUndefined = false
//      The value is definitely assigned and we have valid type information
//
// State 2: Type != Undefined, IsMaybeUndefined = true
//      The value is assigned in one code path, but not in another.
//
// State 3: Type == Undefined, IsMaybeUndefined = true
//      The value is definitely unassigned.
//
// State 4: Type == Undefined, IsMaybeUndefined = false
//      This should never happen as it means the Undefined
//      type has leaked out in an odd way
struct AbstractLocalInfo {
	AbstractValueWithSources ValueInfo;
	bool IsMaybeUndefined;
	/*AbstractValue *Value;
	AbstractSources Loads, Stores;*/

	AbstractLocalInfo() {
		ValueInfo = AbstractValueWithSources();
		IsMaybeUndefined = true;
	}

	AbstractLocalInfo(AbstractValueWithSources valueInfo, bool isUndefined = false) {
		assert(valueInfo.Value != nullptr);
		assert(!(valueInfo.Value == &Undefined && !isUndefined));
		ValueInfo = valueInfo;
		IsMaybeUndefined = isUndefined;
	}

	AbstractLocalInfo merge_with(AbstractLocalInfo other) {
		return AbstractLocalInfo(
			ValueInfo.merge_with(other.ValueInfo),
			IsMaybeUndefined || other.IsMaybeUndefined
		);
	}

	bool operator== (AbstractLocalInfo other) {
		return other.ValueInfo == ValueInfo &&
			other.IsMaybeUndefined == IsMaybeUndefined;
	}
	bool operator!= (AbstractLocalInfo other) {
		return other.ValueInfo != ValueInfo ||
			other.IsMaybeUndefined != IsMaybeUndefined;
	}
};

// Tracks block information for analyzing loops, exception blocks, and break opcodes.
struct AbsIntBlockInfo {
	size_t BlockStart, BlockEnd;
	bool IsLoop;

	AbsIntBlockInfo() {
	}

	AbsIntBlockInfo(size_t blockStart, size_t blockEnd, bool isLoop) {
		BlockStart = blockStart;
		BlockEnd = blockEnd;
		IsLoop = isLoop;
	}
};

// Represents the state of the program at each opcode.  Captures the state of both
// the Python stack and the local variables.  We store the state for each opcode in
// AbstractInterpreter.m_startStates which represents the state before the indexed
// opcode has been executed.
//
// The stack is a unique vector for each interpreter state.  There's currently no
// attempts at sharing because most instructions will alter the value stack.
//
// The locals are shared between InterpreterState's using a shared_ptr because the
// values of locals won't change between most opcodes (via CowVector).  When updating
// a local we first check if the locals are currently shared, and if not simply update
// them in place.  If they are shared then we will issue a copy.
class InterpreterState {
public:
	vector<AbstractValueWithSources> m_stack;
	CowVector<AbstractLocalInfo> m_locals;

	InterpreterState() {
	}

	InterpreterState(int numLocals) {
		m_locals = CowVector<AbstractLocalInfo>(numLocals);
	}

	AbstractLocalInfo get_local(size_t index) {
		return m_locals[index];
	}

	size_t local_count() {
		return m_locals.size();
	}

	void replace_local(size_t index, AbstractLocalInfo value) {
		m_locals.replace(index, value);
	}

	AbstractValue* pop() {
		auto res = m_stack.back();
		res.escapes();
		m_stack.pop_back();
		return res.Value;
	}

	AbstractValueWithSources pop_no_escape() {
		auto res = m_stack.back();
		m_stack.pop_back();
		return res;
	}

	void push(AbstractValueWithSources value) {
		m_stack.push_back(value);
	}

	void push(AbstractValue* value) {
		m_stack.push_back(value);
	}

	size_t stack_size() {
		return m_stack.size();
	}

	AbstractValueWithSources& operator[](const size_t index) {
		return m_stack[index];
	}
};

// The abstract interpreter implementation.  The abstract interpreter performs
// static analysis of the Python byte code to determine what types are known.
// Ultimately this information will feedback into code generation allowing
// more efficient code to be produced.
//
// The abstract interpreter ultimately produces a set of states for each opcode
// before it has been executed.  It also produces an abstract value for the type
// that the function returns.
//
// The abstract interpreter walks the byte code updating the stack of the stack
// and locals based upon the opcode being performed and the existing state of the
// stack.  When it encounters a branch it will merge the current state in with the
// state for where we're branching to.  If the merge results in a new starting state
// that we haven't analyzed it will then queue the target opcode as the next starting
// point to be analyzed.
//
// If the branch is unconditional, or definitively taken based upon analysis, then
// we'll go onto the next starting opcode to be analyzed.
//
// Once we've processed all of the blocks of code in this manner the analysis
// is complete.

#define STACK_KIND_OBJECT true      // A Python object, or a tagged int which might be an object
#define STACK_KIND_VALUE  false     // A non-boxed value, currently just floating point

enum EhFlags {
    EHF_None = 0,
    // The exception handling block includes a continue statement
    EHF_BlockContinues = 0x01,
    // The exception handling block includes a return statement
    EHF_BlockReturns = 0x02,
    // The exception handling block includes a break statement
    EHF_BlockBreaks = 0x04,
    // The exception handling block is in the try portion of a try/finally
    EHF_TryFinally = 0x08,
    // The exception handling block is in the try portion of a try/except
    EHF_TryExcept = 0x10,
    // The exception handling block is in the finally or except portion of a try/finally or try/except
    EHF_InExceptHandler = 0x20,
};

EhFlags operator | (EhFlags lhs, EhFlags rhs);
EhFlags operator |= (EhFlags& lhs, EhFlags rhs);

struct ExceptionVars {
    // The previous exception value before we took the exception we're currently
    // handling.  These correspond with the values in tstate->exc_* and will be
    // restored back to their current values if the exception is handled.
    // When we're generating the try portion of the block these are new locals, when
    // we're generating the finally/except portion of the block these hold the values
    // for the handler so we can unwind from the correct variables.
    Local PrevExc, PrevExcVal, PrevTraceback;
    // The previous traceback and exception values if we're handling a finally block.
    // We store these in locals and keep only the exception type on the stack so that
    // we don't enter the finally handler with multiple stack depths.
    Local FinallyExc, FinallyTb, FinallyValue;

    ExceptionVars() {
    }

    ExceptionVars(IPythonCompiler* comp, bool isFinally = false) {
        PrevExc = comp->emit_define_local(false);
        PrevExcVal = comp->emit_define_local(false);
        PrevTraceback = comp->emit_define_local(false);
        if (isFinally) {
            FinallyExc = comp->emit_define_local(false);
            FinallyTb = comp->emit_define_local(false);
            FinallyValue = comp->emit_define_local(false);
        }
    }
};

// Exception Handling information
struct ExceptionHandler {
    size_t RaiseAndFreeId;
    EhFlags Flags;
    Label Raise,        // our raise stub label, prepares the exception
        ReRaise,        // our re-raise stub label, prepares the exception w/o traceback update
        ErrorTarget;    // The place to branch to for handling errors
    ExceptionVars ExVars;
    vector<bool> EntryStack;
    size_t BackHandler;

    ExceptionHandler(size_t raiseAndFreeId, ExceptionVars exceptionVars, Label raise, Label reraise, Label errorTarget, vector<bool> entryStack, EhFlags flags = EHF_None, size_t backHandler = -1) {
        RaiseAndFreeId = raiseAndFreeId;
        Flags = flags;
        ExVars = exceptionVars;
        EntryStack = entryStack;
        Raise = raise;
        ReRaise = reraise;
        ErrorTarget = errorTarget;
        BackHandler = backHandler;
    }
};

struct BlockInfo {
    int EndOffset, Kind, ContinueOffset;
    EhFlags Flags;
    size_t CurrentHandler;  // the current exception handler, an index into m_allHandlers
    Local LoopVar; //, LoopOpt1, LoopOpt2;

    BlockInfo() {
    }

    BlockInfo(int endOffset, int kind, size_t currentHandler = 0, EhFlags flags = EHF_None, int continueOffset = 0) {
        EndOffset = endOffset;
        Kind = kind;
        Flags = flags;
        CurrentHandler = currentHandler;
        ContinueOffset = continueOffset;
    }
};



class UserModule : public Module {
	Module& m_parent;
public:
	UserModule(Module& parent) : m_parent(parent) {
	}

	virtual IMethod* ResolveMethod(int tokenId) {
		auto res = m_tokenToMethod.find(tokenId);
		if (res == m_tokenToMethod.end()) {
			return m_parent.ResolveMethod(tokenId);
		}

		return res->second;
	}

	virtual int ResolveMethodToken(void* addr) {
		auto res = m_methodAddrToToken.find(addr);
		if (res == m_methodAddrToToken.end()) {
			return m_parent.ResolveMethodToken(addr);
		}
		return res->second;
	}
};

class UserMethod : public IMethod {
	IModule* m_module;
public:
	vector<Parameter> m_params;
	LocalKind m_retType;
	
	UserMethod() {
	}

	~UserMethod() {
		if (m_module != nullptr) {
			delete m_module;
		}
	}

	virtual IModule* get_module() {
		return m_module;
	}

	UserMethod(IModule* module, LocalKind returnType, std::vector<Parameter> params) {
		m_retType = returnType;
		m_params = params;
		m_module = module;
	}

	virtual unsigned int get_param_count() {
		return m_params.size();
	}

	virtual Parameter* get_params() {
		if (m_params.size() == 0) {
			return nullptr;
		}
		return &m_params[0];
	}

	virtual LocalKind get_return_type() {
		return m_retType;
	}
};

#ifdef _WIN32
#define DLL_EXPORT __declspec(dllexport) 
#else
#define DLL_EXPORT
#endif

class DLL_EXPORT AbstractInterpreter {
#pragma warning (disable:4251)
	// ** Results produced:
	// Tracks the interpreter state before each opcode
	unordered_map<size_t, InterpreterState> m_startStates;
	AbstractValue* m_returnValue;
	IMethod* m_method;

	// ** Inputs:
	PyCodeObject* m_code;
	_Py_CODEUNIT *m_byteCode;
	size_t m_size;
	Local m_errorCheckLocal, m_lasti;

	// ** Data consumed during analysis:
	// Tracks whether an END_FINALLY is being consumed by a finally block (true) or exception block (false)
	unordered_map<size_t, bool> m_endFinallyIsFinally;
	// Tracks the entry point for each POP_BLOCK opcode, so we can restore our
	// stack state back after the POP_BLOCK
	unordered_map<size_t, size_t> m_blockStarts;
	// Tracks the location where each BREAK_LOOP will break to, so we can merge
	// state with the current state to the breaked location.
	unordered_map<size_t, AbsIntBlockInfo> m_breakTo;
	unordered_map<size_t, AbstractSource*> m_opcodeSources;
	// all values produced during abstract interpretation, need to be freed
	vector<AbstractValue*> m_values;
	vector<AbstractSource*> m_sources;
	vector<Local> m_raiseAndFreeLocals;
	IPythonCompiler* m_comp;
	// m_blockStack is like Python's f_blockstack which lives on the frame object, except we only maintain
	// it at compile time.  Blocks are pushed onto the stack when we enter a loop, the start of a try block,
	// or into a finally or exception handler.  Blocks are popped as we leave those protected regions.
	// When we pop a block associated with a try body we transform it into the correct block for the handler
	vector<BlockInfo> m_blockStack;
	// All of the exception handlers defined in the method.  After generating the method we'll generate helper
	// targets which dispatch to each of the handlers.
	vector<ExceptionHandler> m_allHandlers;
	// Labels that map from a Python byte code offset to an ilgen label.  This allows us to branch to any
	// byte code offset.
	unordered_map<int, Label> m_offsetLabels;
	// Tracks the depth of the Python stack
	size_t m_blockIds;
	// Tracks the current depth of the stack,  as well as if we have an object reference that needs to be freed.
	// True (STACK_KIND_OBJECT) if we have an object, false (STACK_KIND_VALUE) if we don't
	vector<bool> m_stack;
	// Tracks the state of the stack when we perform a branch.  We copy the existing state to the map and
	// reload it when we begin processing at the stack.
	unordered_map<size_t, vector<bool>> m_offsetStack;
	// Set of labels used for when we need to raise an error but have values on the stack
	// that need to be freed.  We have one set of labels which fall through to each other
	// before doing the raise:
	//      free2: <decref>/<pop>
	//      free1: <decref>/<pop>
	//      raise logic.
	//  This was so we don't need to have decref/frees spread all over the code
	vector<vector<Label>> m_raiseAndFree, m_reraiseAndFree;
	unordered_set<size_t> m_jumpsTo;
	Label m_retLabel;
	Local m_retValue;
	// Stores information for a stack allocated local used for sequence unpacking.  We need to allocate
	// one of these when we enter the method, and we use it if we don't have a sequence we can efficiently
	// unpack.
	unordered_map<int, Local> m_sequenceLocals;
	unordered_map<int, bool> m_assignmentState;
	unordered_map<int, unordered_map<AbstractValueKind, Local>> m_optLocals;
	UserModule *m_module;
#pragma warning (default:4251)

public:
	AbstractInterpreter(PyCodeObject *code, CompilerFactory* compFactory);
	~AbstractInterpreter();

	JittedCode* compile();
	bool interpret();
	void dump();

	void set_local_type(int index, AbstractValueKind kind);
	// Returns information about the specified local variable at a specific
	// byte code index.
	AbstractLocalInfo get_local_info(size_t byteCodeIndex, size_t localIndex);

	// Returns information about the stack at the specific byte code index.
	vector<AbstractValueWithSources>& get_stack_info(size_t byteCodeIndex);

	// Returns true if the result of the opcode should be boxed, false if it
	// can be maintained on the stack.
	bool should_box(size_t opcodeIndex);

	bool can_skip_lasti_update(size_t opcodeIndex);

	AbstractValue* get_return_info();

	bool has_info(size_t byteCodeIndex);

private:
	void emit_lasti_init();
	void emit_lasti_update(int index);
	void load_frame();
	const char * op_to_string(int op);
	void compile_pop_block();
	AbstractValue* to_abstract(PyObject* obj);
	AbstractValue* to_abstract(AbstractValueKind kind);
	bool merge_states(InterpreterState& newState, InterpreterState& mergeTo);
	bool update_start_state(InterpreterState& newState, size_t index);
	void init_starting_state();
	const char* opcode_name(int opcode);
	bool preprocess();
	void dump_sources(AbstractSource* sources);
	AbstractSource* new_source(AbstractSource* source) {
		m_sources.push_back(source);
		return source;
	}

	AbstractSource* add_local_source(size_t opcodeIndex, size_t localIndex);
	AbstractSource* add_const_source(size_t opcodeIndex, size_t constIndex);
	AbstractSource* add_intermediate_source(size_t opcodeIndex);

	void make_function(int oparg);
	void emit_debug_msg(const char * message);
	bool can_skip_lasti_update(int opcodeIndex);
	void build_tuple(size_t argCnt);
	void extend_tuple(size_t argCnt);
	void build_list(size_t argCnt);
	void extend_list_recursively(Local list, size_t argCnt);
	void extend_list(size_t argCnt);
	void build_set(size_t argCnt);
	void extend_set_recursively(Local set, size_t argCnt);
	void extend_set(size_t argCnt);

	void unpack_ex(size_t size, int opcode);

	void build_map(size_t argCnt);
	void extend_map_recursively(Local dict, size_t argCnt);
	void extend_map(size_t argCnt);

	Label getOffsetLabel(int jumpTo);
	void for_iter(int loopIndex, int opcodeIndex, BlockInfo *loopInfo);

	// Checks to see if we have a null value as the last value on our stack
	// indicating an error, and if so, branches to our current error handler.
	void error_check(const char* reason = nullptr);
	void int_error_check(const char* reason = nullptr);

	vector<Label>& get_raise_and_free_labels(size_t blockId);
	vector<Label>& get_reraise_and_free_labels(size_t blockId);
	void ensure_raise_and_free_locals(size_t localCount);
	void emit_raise_and_free(size_t handlerIndex);
	void spill_stack_for_raise(size_t localCount);

	void ensure_labels(vector<Label>& labels, size_t count);

	void branch_raise(const char* reason = nullptr);
	size_t clear_value_stack();
	void raise_on_negative_one();

	void clean_stack_for_reraise();

	void unwind_eh(size_t fromHandler, size_t toHandler = -1);
	void unwind_loop(Local finallyReason, EhFlags branchKind, int branchOffset);

	ExceptionHandler& get_ehblock();

	void mark_offset_label(int index);

	// Frees our iteration temporary variable which gets allocated when we hit
	// a FOR_ITER.  Used when we're breaking from the current loop.
	void free_iter_local();

	void jump_absolute(size_t index, size_t from);

	// Frees all of the iteration variables in a range. Used when we're
	// going to branch to a finally through multiple loops.
	void free_all_iter_locals(size_t to = 0);

	// Frees all of our iteration variables.  Used when we're unwinding the function
	// on an exception.
	void free_iter_locals_on_exception();

	void dec_stack(size_t size = 1);

	void inc_stack(size_t size = 1, bool kind = STACK_KIND_OBJECT);

	// Gets the next opcode skipping for EXTENDED_ARG
	int get_extended_opcode(int curByte);

	// Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a possible error value on the stack.
	// If the value on the stack is -1, we branch to the current error handler.
	// Otherwise branches based if the current value is true/false based upon the current opcode
	void branch_or_error(int& i);

	// Handles POP_JUMP_IF_FALSE/POP_JUMP_IF_TRUE with a bool value known to be on the stack.
	// Branches based if the current value is true/false based upon the current opcode
	void branch(int& i);
	void compare_op(int compareType, int& i, int opcodeIndex);
	JittedCode* compile_worker();

	void periodic_work();
	void store_fast(int local, int opcodeIndex);

	void load_const(int constIndex, int opcodeIndex);

	void return_value(int opcodeIndex);

	void load_fast(int local, int opcodeIndex);
	void load_fast_worker(int local, bool checkUnbound);
	void unpack_sequence(size_t size, int opcode);
	Local get_optimized_local(int index, AbstractValueKind kind);
	void pop_except();

	bool can_optimize_pop_jump(int opcodeIndex);
	void unary_positive(int opcodeIndex);
	void unary_negative(int opcodeIndex);
	void unary_not(int& opcodeIndex);

	void jump_if_or_pop(bool isTrue, int opcodeIndex, int offset);
	void pop_jump_if(bool isTrue, int opcodeIndex, int offset);
	void test_bool_and_branch(Local value, bool isTrue, Label target);

	void debug_log(const char* fmt, ...);

	void emit_new_function();
	void emit_set_closure();
	void emit_set_annotations();
	void emit_set_kw_defaults();
	void emit_set_defaults();
	void emit_load_deref(int index);
	void emit_store_deref(int index);
	void emit_delete_deref(int index);
	void emit_load_closure(int index);
	void emit_set_add();
	void emit_map_add();
	void emit_list_append();

	void emit_raise_varargs();
	void emit_print_expr();
	void emit_load_classderef(int index);
	void emit_getiter();

	void emit_box_bool();
	void emit_box_float();
	void emit_box_tagged_ptr();
	void emit_for_next(Label processValue, Local iterValue);

	void emit_not_in();
	void emit_not_in_push_int();
	void emit_in();
	void emit_in_push_int();
	void emit_is(bool isNot);
	void emit_is_push_int(bool isNot);
	void emit_binary_object(int opcode);
	void emit_binary_tagged_int(int opcode);
	void emit_binary_float(int opcode);

	void emit_compare_tagged_int(int compareType);
	void emit_compare_object(int compareType);
	bool emit_compare_object_push_int(int compareType);
	void emit_periodic_work();

	void emit_unbox_int_tagged();

	void emit_restore_err();
	void emit_compare_exceptions();

	void emit_compare_exceptions_int();
	void emit_pyerr_setstring(void* exception, const char*msg);
	void emit_unwind_eh(Local prevExc, Local prevExcVal, Local prevTraceback);
	void emit_prepare_exception(Local prevExc, Local prevExcVal, Local prevTraceback);

	void emit_unpack_ex(Local sequence, size_t leftSize, size_t rightSize, Local sequenceStorage, Local list, Local remainder);
	void emit_call_args();
	void emit_call_kwargs();
	bool emit_call(size_t argCnt);
	void emit_call_with_tuple();
	bool emit_kwcall(size_t argCnt);
	void emit_kwcall_with_tuple();
	
	template<typename T> inline void call_optimizing_function(T* func) {
		call_optimizing_function((void*)func);
	}
	void call_optimizing_function(void* baseFunction);

	void emit_tagged_int_to_float();

	void emit_unbox_float();
	void emit_tagged_int(size_t value);

	void emit_unbound_local_check();
	void emit_load_fast(int local);
	void emit_store_fast(int local);
	void emit_rot_two(LocalKind kind = LK_Pointer);
	void emit_rot_three(LocalKind kind = LK_Pointer);
	void emit_pop_top();
	void emit_dup_top();
	void emit_dup_top_two();
	void emit_new_list(size_t argCnt);
	void emit_list_store(size_t argCnt);
	void emit_list_extend();
	void emit_list_to_tuple();
	void emit_new_set();
	void emit_pyobject_str();
	void emit_pyobject_repr();
	void emit_pyobject_ascii();
	void emit_pyobject_format();
	void emit_unicode_joinarray();
	void emit_format_value();
	void emit_set_extend();
	void emit_new_dict(size_t size);
	void emit_dict_store();
	void emit_dict_store_no_decref();
	void emit_map_extend();
	void emit_is_true();
	void emit_load_name(void* name);
	void emit_store_name(void* name);
	void emit_delete_name(void* name);
	void emit_store_attr(void* name);
	void emit_delete_attr(void* name);
	void emit_load_attr(void* name);
	void emit_store_global(void* name);
	void emit_delete_global(void* name);
	void emit_load_global(void* name);
	void emit_delete_fast(int index);
	void emit_new_tuple(size_t size);
	void emit_tuple_load(size_t index);
	void emit_tuple_store(size_t argCnt);
	void emit_store_subscr();
	void emit_delete_subscr();
	void emit_build_slice();
	void emit_unary_positive();
	void emit_unary_negative();
	void emit_unary_not_push_int();
	void emit_unary_not();
	void emit_unary_negative_float();
	void emit_unary_negative_tagged_int();
	void emit_unary_not_float_push_bool();
	void emit_unary_not_tagged_int_push_bool();
	void emit_unary_invert();
	void emit_import_name(void* name);
	void emit_import_from(void* name);
	void emit_import_star();
	void emit_load_build_class();
	void emit_unpack_sequence(Local sequence, Local sequenceStorage, Label success, size_t size);

	void decref();

	void emit_push_frame();
	void emit_pop_frame();
	void emit_eh_trace();
	void load_local(int oparg);
	void emit_incref(bool maybeTagged = false);
};

class IndirectDispatchMethod : public IMethod {
	IMethod* m_coreMethod;
public:
	void* m_addr;

	IndirectDispatchMethod(IMethod* coreMethod) : m_coreMethod(coreMethod) {
		m_addr = m_coreMethod->get_addr();
	}

public:
	virtual IModule* get_module() {
		return nullptr;
	}

	virtual void* get_addr() {
		return m_addr;
	}

	virtual void* get_indirect_addr() {
		return &m_addr;
	}

	virtual unsigned int get_param_count() {
		return m_coreMethod->get_param_count();
	}

	virtual Parameter* get_params() {
		return m_coreMethod->get_params();
	}

	virtual LocalKind get_return_type() {
		return m_coreMethod->get_return_type();
	}
};


#endif
