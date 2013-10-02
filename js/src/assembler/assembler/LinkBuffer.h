/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 * 
 * ***** END LICENSE BLOCK ***** */

#ifndef assembler_assembler_LinkBuffer_h
#define assembler_assembler_LinkBuffer_h

#include "assembler/wtf/Platform.h"

#if ENABLE_ASSEMBLER

#include "assembler/assembler/MacroAssembler.h"

namespace JSC {

// LinkBuffer:
//
// This class assists in linking code generated by the macro assembler, once code generation
// has been completed, and the code has been copied to is final location in memory.  At this
// time pointers to labels within the code may be resolved, and relative offsets to external
// addresses may be fixed.
//
// Specifically:
//   * Jump objects may be linked to external targets,
//   * The address of Jump objects may taken, such that it can later be relinked.
//   * The return address of a Call may be acquired.
//   * The address of a Label pointing into the code may be resolved.
//   * The value referenced by a DataLabel may be set.
//
class LinkBuffer {
    typedef MacroAssemblerCodeRef CodeRef;
    typedef MacroAssembler::Label Label;
    typedef MacroAssembler::Jump Jump;
    typedef MacroAssembler::JumpList JumpList;
    typedef MacroAssembler::Call Call;
    typedef MacroAssembler::DataLabel32 DataLabel32;
    typedef MacroAssembler::DataLabelPtr DataLabelPtr;

public:
    // 'ok' should be checked after this constructor is called;  it's false if OOM occurred.
    LinkBuffer(MacroAssembler* masm, ExecutableAllocator* executableAllocator,
               ExecutablePool** poolp, bool* ok, CodeKind codeKind)
    {
        m_codeKind = codeKind;
        m_code = executableAllocAndCopy(*masm, executableAllocator, poolp);
        m_executablePool = *poolp;
        m_size = masm->m_assembler.size();  // must come after call to executableAllocAndCopy()!
#ifndef NDEBUG
        m_completed = false;
#endif
        *ok = !!m_code;
    }

    LinkBuffer(CodeKind kind)
        : m_executablePool(NULL)
        , m_code(NULL)
        , m_size(0)
        , m_codeKind(kind)
#ifndef NDEBUG
        , m_completed(false)
#endif
    {
    }

    LinkBuffer(uint8_t* ncode, size_t size, CodeKind kind)
        : m_executablePool(NULL)
        , m_code(ncode)
        , m_size(size)
        , m_codeKind(kind)
#ifndef NDEBUG
        , m_completed(false)
#endif
    {
    }

    ~LinkBuffer()
    {
        ASSERT(!m_executablePool || m_completed);
    }

    // These methods are used to link or set values at code generation time.

    void link(Call call, FunctionPtr function)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        MacroAssembler::linkCall(code(), call, function);
    }
    
    void link(Jump jump, CodeLocationLabel label)
    {
        MacroAssembler::linkJump(code(), jump, label);
    }

    void link(JumpList list, CodeLocationLabel label)
    {
        for (unsigned i = 0; i < list.m_jumps.length(); ++i)
            MacroAssembler::linkJump(code(), list.m_jumps[i], label);
    }

    void patch(DataLabelPtr label, void* value)
    {
        MacroAssembler::linkPointer(code(), label.m_label, value);
    }

    void patch(DataLabelPtr label, CodeLocationLabel value)
    {
        MacroAssembler::linkPointer(code(), label.m_label, value.executableAddress());
    }

    // These methods are used to obtain handles to allow the code to be relinked / repatched later.

    CodeLocationCall locationOf(Call call)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        ASSERT(!call.isFlagSet(Call::Near));
        return CodeLocationCall(MacroAssembler::getLinkerAddress(code(), call.m_jmp));
    }

    CodeLocationJump locationOf(Jump j)
    {
        return CodeLocationJump(MacroAssembler::getLinkerAddress(code(), j.m_jmp));
    }

    CodeLocationNearCall locationOfNearCall(Call call)
    {
        ASSERT(call.isFlagSet(Call::Linkable));
        ASSERT(call.isFlagSet(Call::Near));
        return CodeLocationNearCall(MacroAssembler::getLinkerAddress(code(), call.m_jmp));
    }

    CodeLocationLabel locationOf(Label label)
    {
        return CodeLocationLabel(MacroAssembler::getLinkerAddress(code(), label.m_label));
    }

    CodeLocationDataLabelPtr locationOf(DataLabelPtr label)
    {
        return CodeLocationDataLabelPtr(MacroAssembler::getLinkerAddress(code(), label.m_label));
    }

    CodeLocationDataLabel32 locationOf(DataLabel32 label)
    {
        return CodeLocationDataLabel32(MacroAssembler::getLinkerAddress(code(), label.m_label));
    }

    // This method obtains the return address of the call, given as an offset from
    // the start of the code.
    unsigned returnAddressOffset(Call call)
    {
        return MacroAssembler::getLinkerCallReturnOffset(call);
    }

    // Upon completion of all patching either 'finalizeCode()' or 'finalizeCodeAddendum()' should be called
    // once to complete generation of the code.  'finalizeCode()' is suited to situations
    // where the executable pool must also be retained, the lighter-weight 'finalizeCodeAddendum()' is
    // suited to adding to an existing allocation.
    CodeRef finalizeCode()
    {
        performFinalization();

        return CodeRef(m_code, m_executablePool, m_size);
    }
    CodeLocationLabel finalizeCodeAddendum()
    {
        performFinalization();

        return CodeLocationLabel(code());
    }

    // Useful as a proxy to detect OOM.
    void* unsafeCode() {
        return code();
    }

protected:
    // Keep this private! - the underlying code should only be obtained externally via 
    // finalizeCode() or finalizeCodeAddendum().
    void* code()
    {
        return m_code;
    }

    void *executableAllocAndCopy(MacroAssembler &masm, ExecutableAllocator *allocator,
                                 ExecutablePool **poolp)
    {
        return masm.m_assembler.executableAllocAndCopy(allocator, poolp, m_codeKind);
    }

    void performFinalization()
    {
#ifndef NDEBUG
        ASSERT(!m_completed);
        m_completed = true;
#endif

        ExecutableAllocator::makeExecutable(code(), m_size);
        ExecutableAllocator::cacheFlush(code(), m_size);
    }

    ExecutablePool* m_executablePool;
    void* m_code;
    size_t m_size;
    CodeKind m_codeKind;
#ifndef NDEBUG
    bool m_completed;
#endif
};

} // namespace JSC

#endif // ENABLE(ASSEMBLER)

#endif /* assembler_assembler_LinkBuffer_h */
