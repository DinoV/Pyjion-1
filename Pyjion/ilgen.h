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

#ifndef ILGEN_H
#define ILGEN_H

#define FEATURE_NO_HOST

#include <stdint.h>
#include <wchar.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <float.h>
#include <share.h>
#include <cstdlib>
#include <intrin.h>

#include <corjit.h>
#include <openum.h>

#include "ipycomp.h"
#include "bridge.h"

extern const signed char    opcodeSizes[];
extern const char * const   opcodeNames[];
extern const BYTE           opcodeArgKinds[];

template<typename T> struct simple_vector {
	T* m_items;
	size_t m_count, m_allocated;

public:
	simple_vector() {
		m_items = nullptr;
		m_count = m_allocated = 0;
	}

	size_t size() {
		return m_count;
	}

	T& operator[](size_t n) {
		return m_items[n];
	}

	void push_back(T item) {
		if (m_count >= m_allocated) {
			auto newCount = max(4, m_allocated * 2);
			T* newItems = (T*)malloc(sizeof(T) * newCount);

			if (m_items != nullptr) {
				memcpy(newItems, m_items, m_count * sizeof(T));
				free(m_items);
			}

			m_items = newItems;
			m_allocated = newCount;
		}
		m_items[m_count++] = item;
	}

	void pop_back() {
		_ASSERTE(m_count > 0);
		m_count--;
	}
};

class LabelInfo {
public:
	int m_location;
	simple_vector<int> m_branchOffsets;

	LabelInfo() {
		m_location = -1;
	}
};

class ILGenerator {
	simple_vector<Parameter> m_locals;
	simple_vector<Local> m_freedLocals[CORINFO_TYPE_COUNT];
	IMethod* m_method;

public:
	simple_vector<BYTE> m_il;
	int m_localCount;
	simple_vector<LabelInfo> m_labels;

public:
	ILGenerator(IMethod* method) {
		m_method = method;
		m_localCount = 0;
	}

	Local define_local(Parameter param) {
		auto& existing = m_freedLocals[param.m_type];
		if (existing.size() != 0) {
			auto res = existing[existing.size() - 1];
			existing.pop_back();
			return res;
		}
		return define_local_no_cache(param);
	}

	Local define_local_no_cache(Parameter param) {
		m_locals.push_back(param);
		return Local(m_localCount++);
	}

	void free_local(Local local) {
		auto param = m_locals[local.m_index];
		auto& localList = m_freedLocals[param.m_type];
#if _DEBUG
		for (int i = 0; i < localList.size(); i++) {
			if (localList[i].m_index == local.m_index) {
				// locals shouldn't be double freed...
				assert(FALSE);
			}
		}
#endif
		localList.push_back(local);
	}

	Label define_label() {
		m_labels.push_back(LabelInfo());
		return Label((int)m_labels.size() - 1);
	}

	void mark_label(Label label) {
		auto info = &m_labels[label.m_index];
		_ASSERTE(info->m_location == -1);
		info->m_location = (int)m_il.size();
		for (int i = 0; i < info->m_branchOffsets.size(); i++) {
			auto from = info->m_branchOffsets[i];
			auto offset = info->m_location - (from + 4);		// relative to the end of the instruction

			m_il[from] = offset & 0xFF;
			m_il[from + 1] = (offset >> 8) & 0xFF;
			m_il[from + 2] = (offset >> 16) & 0xFF;
			m_il[from + 3] = (offset >> 24) & 0xFF;
		}
	}

	void localloc() {
		push_back(CEE_PREFIX1);
		push_back((char)CEE_LOCALLOC);
	}

	void ret() {
		push_back(CEE_RET);
	}

	void ld_r8(double i) {
		push_back(CEE_LDC_R8);
		unsigned char* value = (unsigned char*)(&i);
		for (int i = 0; i < 8; i++) {
			m_il.push_back(value[i]);
		}
	}

	void ld_i4(int i) {
		switch (i) {
		case -1:push_back(CEE_LDC_I4_M1); break;
		case 0: push_back(CEE_LDC_I4_0); break;
		case 1: push_back(CEE_LDC_I4_1); break;
		case 2: push_back(CEE_LDC_I4_2); break;
		case 3: push_back(CEE_LDC_I4_3); break;
		case 4: push_back(CEE_LDC_I4_4); break;
		case 5: push_back(CEE_LDC_I4_5); break;
		case 6: push_back(CEE_LDC_I4_6); break;
		case 7: push_back(CEE_LDC_I4_7); break;
		default:
			if (i < 256) {
				push_back(CEE_LDC_I4_S);
				m_il.push_back(i);

			}
			else {
				m_il.push_back(CEE_LDC_I4);
				m_il.push_back((char)CEE_STLOC);
				emit_int(i);
			}
		}
	}

	void load_null() {
		ld_i4(0);
		m_il.push_back(CEE_CONV_I);
	}

	void st_ind_i() {
		push_back(CEE_STIND_I);
	}

	void ld_ind_i() {
		push_back(CEE_LDIND_I);
	}

	void st_ind_i4() {
		push_back(CEE_STIND_I4);
	}

	void ld_ind_i4() {
		push_back(CEE_LDIND_I4);
	}

	void ld_ind_r8() {
		push_back(CEE_LDIND_R8);
	}

	void branch(BranchType branchType, Label label) {
		auto info = &m_labels[label.m_index];
		if (info->m_location == -1) {
			info->m_branchOffsets.push_back((int)m_il.size() + 1);
			branch(branchType, 0xFFFF);
		}
		else {
			branch(branchType, (int)(info->m_location - m_il.size()));
		}
	}

	void branch(BranchType branchType, int offset) {
		if ((offset - 2) <= 128 && (offset - 2) >= -127) {
			switch (branchType) {
			case BranchLeave:
				m_il.push_back(CEE_LEAVE_S);
				break;
			case BranchAlways:
				m_il.push_back(CEE_BR_S);
				break;
			case BranchTrue:
				m_il.push_back(CEE_BRTRUE_S);
				break;
			case BranchFalse:
				m_il.push_back(CEE_BRFALSE_S);
				break;
			case BranchEqual:
				m_il.push_back(CEE_BEQ_S);
				break;
			case BranchNotEqual:
				m_il.push_back(CEE_BNE_UN_S);
				break;
			}
			m_il.push_back((char)offset - 2);
		}
		else {
			switch (branchType) {
			case BranchLeave:
				m_il.push_back(CEE_LEAVE);
				break;
			case BranchAlways:
				m_il.push_back(CEE_BR);
				break;
			case BranchTrue:
				m_il.push_back(CEE_BRTRUE);
				break;
			case BranchFalse:
				m_il.push_back(CEE_BRFALSE);
				break;
			case BranchEqual:
				m_il.push_back(CEE_BEQ);
				break;
			case BranchNotEqual:
				m_il.push_back(CEE_BNE_UN);
				break;
			}
			emit_int(offset - 5);
		}
	}

	void neg() {
		m_il.push_back(CEE_NEG);
	}

	void dup() {
		m_il.push_back(CEE_DUP);
	}

	void bitwise_and() {
		m_il.push_back(CEE_AND);
	}

	void pop() {
		m_il.push_back(CEE_POP);
	}

	void compare_eq() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CEQ);
	}

	void compare_ne() {
		compare_eq();
		ld_i4(0);
		compare_eq();
	}

	void compare_gt() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CGT);
	}

	void compare_lt() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CLT);
	}

	void compare_ge() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CLT);
		ld_i4(0);
		compare_eq();
	}

	void compare_le() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CGT);
		ld_i4(0);
		compare_eq();
	}

	void compare_ge_float() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CLT_UN);
		ld_i4(0);
		compare_eq();
	}

	void compare_le_float() {
		m_il.push_back(CEE_PREFIX1);
		m_il.push_back((char)CEE_CGT_UN);
		ld_i4(0);
		compare_eq();
	}

	void ld_i(int i) {
		m_il.push_back(CEE_LDC_I4);
		emit_int(i);
		m_il.push_back(CEE_CONV_I);
	}

	void ld_i(size_t i) {
		ld_i((void*)i);
	}

	void ld_i(void* ptr) {
		size_t value = (size_t)ptr;
#ifdef _TARGET_AMD64_
		if ((value & 0xFFFFFFFF) == value) {
			ld_i((int)value);
		}
		else {
			m_il.push_back(CEE_LDC_I8);
			m_il.push_back(value & 0xff);
			m_il.push_back((value >> 8) & 0xff);
			m_il.push_back((value >> 16) & 0xff);
			m_il.push_back((value >> 24) & 0xff);
			m_il.push_back((value >> 32) & 0xff);
			m_il.push_back((value >> 40) & 0xff);
			m_il.push_back((value >> 48) & 0xff);
			m_il.push_back((value >> 56) & 0xff);
			m_il.push_back(CEE_CONV_I);
		}
#else
		ld_i(value);
		m_il.push_back(CEE_CONV_I);
#endif
	}

	void emit_call(int token) {
		m_il.push_back(CEE_CALL);
		emit_int(token);
	}

	//void emit_calli(int token) {
	//	m_il.push_back(CEE_CALLI);
	//	emit_int(token);
	//}

	//void emit_callvirt(int token) {
	//	m_il.push_back(CEE_CALLVIRT);
	//	emit_int(token);
	//}

	void st_loc(Local param) {
		st_loc(param.m_index);
	}

	void ld_loc(Local param) {
		ld_loc(param.m_index);
	}

	void ld_loca(Local param) {
		_ASSERTE(param.is_valid());
		ld_loca(param.m_index);
	}

	void st_loc(int index) {
		_ASSERTE(index != -1);
		switch (index) {
		case 0: m_il.push_back(CEE_STLOC_0); break;
		case 1: m_il.push_back(CEE_STLOC_1); break;
		case 2: m_il.push_back(CEE_STLOC_2); break;
		case 3: m_il.push_back(CEE_STLOC_3); break;
		default:
			if (index < 256) {
				m_il.push_back(CEE_STLOC_S);
				m_il.push_back(index);
			}
			else {
				m_il.push_back(CEE_PREFIX1);
				m_il.push_back((char)CEE_STLOC);
				m_il.push_back(index & 0xff);
				m_il.push_back((index >> 8) & 0xff);
			}
		}
	}

	void ld_loc(int index) {
		_ASSERTE(index != -1);
		switch (index) {
		case 0: m_il.push_back(CEE_LDLOC_0); break;
		case 1: m_il.push_back(CEE_LDLOC_1); break;
		case 2: m_il.push_back(CEE_LDLOC_2); break;
		case 3: m_il.push_back(CEE_LDLOC_3); break;
		default:
			if (index < 256) {
				m_il.push_back(CEE_LDLOC_S);
				m_il.push_back(index);
			}
			else {
				m_il.push_back(CEE_PREFIX1);
				m_il.push_back((char)CEE_LDLOC);
				m_il.push_back(index & 0xff);
				m_il.push_back((index >> 8) & 0xff);
			}
		}
	}

	void ld_loca(int index) {
		_ASSERTE(index != -1);
		if (index < 256) {
			m_il.push_back(CEE_LDLOCA_S);
			m_il.push_back(index);
		}
		else {
			m_il.push_back(CEE_PREFIX1);
			m_il.push_back((char)CEE_LDLOCA);
			m_il.push_back(index & 0xff);
			m_il.push_back((index >> 8) & 0xff);
		}
	}

	CORINFO_METHOD_INFO to_method(int stackSize) {
		CORINFO_METHOD_INFO methodInfo;
		methodInfo.ftn = (CORINFO_METHOD_HANDLE)m_method;
		methodInfo.scope = (CORINFO_MODULE_HANDLE)m_method->get_module();
		methodInfo.ILCode = &m_il[0];
		methodInfo.ILCodeSize = (unsigned int)m_il.size();
		methodInfo.maxStack = stackSize;
		methodInfo.EHcount = 0;
		methodInfo.options = CORINFO_OPT_INIT_LOCALS;
		methodInfo.regionKind = CORINFO_REGION_JIT;
		methodInfo.args = CORINFO_SIG_INFO{ CORINFO_CALLCONV_DEFAULT };
		methodInfo.args.args = (CORINFO_ARG_LIST_HANDLE)(m_method->get_param_count() == 0 ? nullptr : &m_method->get_params()[0]);
		methodInfo.args.numArgs = m_method->get_param_count();
		methodInfo.args.retType = to_clr_type(m_method->get_return_type());
		methodInfo.args.retTypeClass = nullptr;
		methodInfo.locals = CORINFO_SIG_INFO{ CORINFO_CALLCONV_DEFAULT };
		methodInfo.locals.args = (CORINFO_ARG_LIST_HANDLE)(m_locals.size() == 0 ? nullptr : &m_locals[0]);
		methodInfo.locals.numArgs = m_locals.size();
		return methodInfo;
	}

	void* compile(ICorJitInfo* jitInfo, ICorJitCompiler* jit, int stackSize) {
		BYTE* nativeEntry;
		ULONG nativeSizeOfCode;
		CORINFO_METHOD_INFO methodInfo = to_method(stackSize);
		CorJitResult result = jit->compileMethod(
			/*ICorJitInfo*/jitInfo,
			/*CORINFO_METHOD_INFO */&methodInfo,
			/*flags*/CORJIT_FLAGS::CORJIT_FLAG_CALL_GETJITFLAGS,
			&nativeEntry,
			&nativeSizeOfCode
		);
		
		if (result == CORJIT_OK) {
			return nativeEntry;
		}
		return nullptr;
	}

	void add() {
		push_back(CEE_ADD);
	}

	void sub() {
		push_back(CEE_SUB);
	}

	void div() {
		push_back(CEE_DIV);
	}

	void mod() {
		push_back(CEE_REM);
	}

	void mul() {
		push_back(CEE_MUL);
	}

	void ld_arg(int index) {
		_ASSERTE(index != -1);
		switch (index) {
		case 0: push_back(CEE_LDARG_0); break;
		case 1: push_back(CEE_LDARG_1); break;
		case 2: push_back(CEE_LDARG_2); break;
		case 3: push_back(CEE_LDARG_3); break;
		default:
			if (index < 256) {
				push_back(CEE_LDARG_3);
				m_il.push_back(index);
			}
			else {
				m_il.push_back(CEE_PREFIX1);
				m_il.push_back((char)CEE_LDARG);
				m_il.push_back(index & 0xff);
				m_il.push_back((index >> 8) & 0xff);
			}

			break;
		}
	}
private:
	void emit_int(int value) {
		m_il.push_back(value & 0xff);
		m_il.push_back((value >> 8) & 0xff);
		m_il.push_back((value >> 16) & 0xff);
		m_il.push_back((value >> 24) & 0xff);
	}

	void push_back(BYTE b) {
		m_il.push_back(b);
	}

	void dump() {
		for (auto i = 0; i < m_locals.size(); i++) {
			pyjit_log("Local %d: %s\n", i, LK_ToString(m_locals[i].m_type));
		}
		dumpILRange(&m_il[0], m_il.size());
	}

	void dumpILRange(const BYTE* const codeAddr, unsigned codeSize) {
		for (size_t offs = 0; offs < codeSize; ) {
			char prefix[100];
			sprintf_s(prefix, sizeof(prefix), "IL_%04x ", offs);
			unsigned codeBytesDumped = dumpSingleInstr(codeAddr, offs, prefix);
			offs += codeBytesDumped;
		}
	}

	void dumpILBytes(const BYTE* const codeAddr, unsigned codeSize, unsigned  alignSize) {
		for (size_t offs = 0; offs < codeSize; ++offs) {
			pyjit_log(" %02x", *(codeAddr + offs));
		}

		unsigned charsWritten = 3 * codeSize;
		for (unsigned i = charsWritten; i < alignSize; i++) {
			pyjit_log(" ");
		}
	}


	inline unsigned __int8 getU1LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED unsigned __int8 *)ptr;
	}

	inline unsigned __int16    getU2LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED unsigned __int16 *)ptr;
	}

	inline unsigned __int32    getU4LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED unsigned __int32*)ptr;
	}

	inline signed __int8     getI1LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED signed __int8 *)ptr;
	}

	inline signed __int16    getI2LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED signed __int16 *)ptr;
	}

	inline signed __int32    getI4LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED signed __int32*)ptr;
	}

	inline signed __int64    getI8LittleEndian(const BYTE * ptr) {
		return *(UNALIGNED signed __int64*)ptr;
	}

	inline float               getR4LittleEndian(const BYTE * ptr) {
		__int32 val = getI4LittleEndian(ptr);
		return *(float *)&val;
	}

	inline double              getR8LittleEndian(const BYTE * ptr) {
		__int64 val = getI8LittleEndian(ptr);
		return *(double *)&val;
	}


	//------------------------------------------------------------------------
	// dumpSingleInstr: Display a single IL instruction.
	//
	// Arguments:
	//    codeAddr  - Base pointer to a stream of IL instructions.
	//    offs      - Offset from codeAddr of the IL instruction to display.
	//    prefix    - Optional string to prefix the IL instruction with (if nullptr, no prefix is output).
	// 
	// Return Value:
	//    Size of the displayed IL instruction in the instruction stream, in bytes. (Add this to 'offs' to
	//    get to the next instruction.)
	// 
	unsigned
		dumpSingleInstr(const BYTE* const codeAddr, size_t offs, const char* prefix)
	{
		const BYTE  *        opcodePtr = codeAddr + offs;
		const BYTE  *   startOpcodePtr = opcodePtr;
		const unsigned ALIGN_WIDTH = 3 * 9; // assume 3 characters * (1 byte opcode + 8 bytes data) for most the things

		if (prefix != NULL)
			pyjit_log("%s", prefix);

		OPCODE      opcode = (OPCODE)getU1LittleEndian(opcodePtr);
		opcodePtr += 1;

	DECODE_OPCODE:

		if (opcode >= CEE_COUNT)
		{
			pyjit_log("\nIllegal opcode: %02X\n", (int)opcode);
			return (size_t)(opcodePtr - startOpcodePtr);
		}

		/* Get the size of additional parameters */

		size_t      sz = opcodeSizes[opcode];
		unsigned    argKind = opcodeArgKinds[opcode];

		/* See what kind of an opcode we have, then */

		switch (opcode)
		{
		case CEE_PREFIX1:
			opcode = OPCODE(getU1LittleEndian(opcodePtr) + 256);
			opcodePtr += sizeof(__int8);
			goto DECODE_OPCODE;
		case CEE_CALL:
		{
			dumpILBytes(startOpcodePtr, (unsigned)((opcodePtr - startOpcodePtr) + sz), ALIGN_WIDTH);
			auto method = getI4LittleEndian(opcodePtr);
			auto methodObj = this->m_method->get_module()->ResolveMethod(method);
			const char * name = methodObj->get_name();
			if (name != nullptr) {
				pyjit_log(" %-12s %s ", opcodeNames[opcode], name);
				pyjit_log(" (pops %d, pushes %d)", methodObj->get_param_count(), methodObj->get_return_type() == LK_Void ? 0 : 1);
				opcodePtr += 4;
			}
			else {
				goto no_method_name;
			}
			break;
		}
		default:
		{
		no_method_name:
			__int64     iOp;
			double      dOp;
			int         jOp;
			DWORD       jOp2;

			switch (argKind)
			{
			case InlineNone:
				dumpILBytes(startOpcodePtr, (unsigned)(opcodePtr - startOpcodePtr), ALIGN_WIDTH);
				pyjit_log(" %-12s", opcodeNames[opcode]);
				break;

			case ShortInlineVar:   iOp = getU1LittleEndian(opcodePtr);  goto INT_OP;
			case ShortInlineI:   iOp = getI1LittleEndian(opcodePtr);  goto INT_OP;
			case InlineVar:   iOp = getU2LittleEndian(opcodePtr);  goto INT_OP;
			case InlineTok:
			case InlineMethod:
			case InlineField:
			case InlineType:
			case InlineString:
			case InlineSig:
			case InlineI:   iOp = getI4LittleEndian(opcodePtr);  goto INT_OP;
			case InlineI8:   iOp = getU4LittleEndian(opcodePtr);
				iOp |= (__int64)getU4LittleEndian(opcodePtr + 4) << 32;
				dumpILBytes(startOpcodePtr, (unsigned)((opcodePtr - startOpcodePtr) + sz), ALIGN_WIDTH);
				pyjit_log(" %-12s 0x%llX", opcodeNames[opcode], iOp);
				break;

			INT_OP:
				dumpILBytes(startOpcodePtr, (unsigned)((opcodePtr - startOpcodePtr) + sz), ALIGN_WIDTH);
				pyjit_log(" %-12s 0x%X", opcodeNames[opcode], iOp);
				break;

			case ShortInlineR:  dOp = getR4LittleEndian(opcodePtr);  goto FLT_OP;
			case InlineR:  dOp = getR8LittleEndian(opcodePtr);  goto FLT_OP;

			FLT_OP:
				dumpILBytes(startOpcodePtr, (unsigned)((opcodePtr - startOpcodePtr) + sz), ALIGN_WIDTH);
				pyjit_log(" %-12s %f", opcodeNames[opcode], dOp);
				break;

			case ShortInlineBrTarget:  jOp = getI1LittleEndian(opcodePtr);  goto JMP_OP;
			case InlineBrTarget:       jOp = getI4LittleEndian(opcodePtr);  goto JMP_OP;

			JMP_OP:
				dumpILBytes(startOpcodePtr, (unsigned)((opcodePtr - startOpcodePtr) + sz), ALIGN_WIDTH);
				pyjit_log(" %-12s %d (IL_%04x)",
					opcodeNames[opcode],
					jOp,
					(int)(opcodePtr + sz - codeAddr) + jOp);
				break;

			case InlineSwitch:
				jOp2 = getU4LittleEndian(opcodePtr);
				opcodePtr += 4;
				opcodePtr += jOp2 * 4; // Jump over the table
				dumpILBytes(startOpcodePtr, (unsigned)(opcodePtr - startOpcodePtr), ALIGN_WIDTH);
				pyjit_log(" %-12s", opcodeNames[opcode]);
				break;

			case InlinePhi:
				jOp2 = getU1LittleEndian(opcodePtr);
				opcodePtr += 1;
				opcodePtr += jOp2 * 2; // Jump over the table
				dumpILBytes(startOpcodePtr, (unsigned)(opcodePtr - startOpcodePtr), ALIGN_WIDTH);
				pyjit_log(" %-12s", opcodeNames[opcode]);
				break;

			default: assert(!"Bad argKind");
			}

			opcodePtr += sz;
			break;
		}
		}

		pyjit_log("\n");
		return (size_t)(opcodePtr - startOpcodePtr);
	}

};

#endif