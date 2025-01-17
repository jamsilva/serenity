/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/JIT/NativeExecutable.h>
#include <LibJS/Runtime/VM.h>
#include <LibX86/Disassembler.h>
#include <sys/mman.h>

#if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define EXECINFO_BACKTRACE
#endif

#if defined(AK_OS_ANDROID) && (__ANDROID_API__ < 33)
#    undef EXECINFO_BACKTRACE
#endif

namespace JS::JIT {

NativeExecutable::NativeExecutable(void* code, size_t size, Vector<BytecodeMapping> mapping)
    : m_code(code)
    , m_size(size)
    , m_mapping(move(mapping))
{
}

NativeExecutable::~NativeExecutable()
{
    munmap(m_code, m_size);
}

void NativeExecutable::run(VM& vm) const
{
    typedef void (*JITCode)(VM&, Value* registers, Value* locals);
    ((JITCode)m_code)(vm,
        vm.bytecode_interpreter().registers().data(),
        vm.running_execution_context().local_variables.data());
}

#if ARCH(X86_64)
class JITSymbolProvider : public X86::SymbolProvider {
public:
    JITSymbolProvider(NativeExecutable const& executable)
        : m_executable(executable)
    {
    }

    virtual ~JITSymbolProvider() override = default;

    virtual DeprecatedString symbolicate(FlatPtr address, u32* offset = nullptr) const override
    {
        auto base = bit_cast<FlatPtr>(m_executable.code_bytes().data());
        auto native_offset = static_cast<u32>(address - base);
        if (native_offset >= m_executable.code_bytes().size())
            return {};

        auto const& entry = m_executable.find_mapping_entry(native_offset);

        if (offset)
            *offset = native_offset - entry.native_offset;

        if (entry.block_index == BytecodeMapping::EXECUTABLE)
            return BytecodeMapping::EXECUTABLE_LABELS[entry.bytecode_offset];

        if (entry.bytecode_offset == 0)
            return DeprecatedString::formatted("Block {}", entry.block_index + 1);

        return DeprecatedString::formatted("{}:{:x}", entry.block_index + 1, entry.bytecode_offset);
    }

private:
    NativeExecutable const& m_executable;
};
#endif

void NativeExecutable::dump_disassembly([[maybe_unused]] Bytecode::Executable const& executable) const
{
#if ARCH(X86_64)
    auto const* code_bytes = static_cast<u8 const*>(m_code);
    auto stream = X86::SimpleInstructionStream { code_bytes, m_size };
    auto disassembler = X86::Disassembler(stream);
    auto symbol_provider = JITSymbolProvider(*this);
    auto mapping = m_mapping.begin();

    auto first_instruction = Bytecode::InstructionStreamIterator { executable.basic_blocks[0]->instruction_stream(), &executable };
    auto source_range = first_instruction.source_range().realize();
    dbgln("Disassembly of '{}' ({}:{}:{}):", executable.name, source_range.filename(), source_range.start.line, source_range.start.column);

    while (true) {
        auto offset = stream.offset();
        auto virtual_offset = bit_cast<size_t>(m_code) + offset;

        while (!mapping.is_end() && offset > mapping->native_offset)
            ++mapping;
        if (!mapping.is_end() && offset == mapping->native_offset) {
            if (mapping->block_index == BytecodeMapping::EXECUTABLE) {
                dbgln("{}:", BytecodeMapping::EXECUTABLE_LABELS[mapping->bytecode_offset]);
            } else {
                auto const& block = *executable.basic_blocks[mapping->block_index];
                if (mapping->bytecode_offset == 0)
                    dbgln("\nBlock {}:", mapping->block_index + 1);

                VERIFY(mapping->bytecode_offset < block.size());
                auto const& instruction = *reinterpret_cast<Bytecode::Instruction const*>(block.data() + mapping->bytecode_offset);
                dbgln("{}:{:x} {}:", mapping->block_index + 1, mapping->bytecode_offset, instruction.to_deprecated_string(executable));
            }
        }

        auto insn = disassembler.next();
        if (!insn.has_value())
            break;

        StringBuilder builder;
        builder.appendff("{:p}  ", virtual_offset);
        auto length = insn.value().length();
        for (size_t i = 0; i < 7; i++) {
            if (i < length)
                builder.appendff("{:02x} ", code_bytes[offset + i]);
            else
                builder.append("   "sv);
        }
        builder.append(" "sv);
        builder.append(insn.value().to_deprecated_string(virtual_offset, &symbol_provider));
        dbgln("{}", builder.string_view());

        for (size_t bytes_printed = 7; bytes_printed < length; bytes_printed += 7) {
            builder.clear();
            builder.appendff("{:p} ", virtual_offset + bytes_printed);
            for (size_t i = bytes_printed; i < bytes_printed + 7 && i < length; i++)
                builder.appendff(" {:02x}", code_bytes[offset + i]);
            dbgln("{}", builder.string_view());
        }
    }

    dbgln();
#endif
}

BytecodeMapping const& NativeExecutable::find_mapping_entry(size_t native_offset) const
{
    size_t nearby_index = 0;
    AK::binary_search(
        m_mapping,
        native_offset,
        &nearby_index,
        [](FlatPtr needle, BytecodeMapping const& mapping_entry) {
            if (needle > mapping_entry.native_offset)
                return 1;
            if (needle == mapping_entry.native_offset)
                return 0;
            return -1;
        });
    return m_mapping[nearby_index];
}

Optional<Bytecode::InstructionStreamIterator const&> NativeExecutable::instruction_stream_iterator([[maybe_unused]] Bytecode::Executable const& executable) const
{
#ifdef EXECINFO_BACKTRACE
    void* buffer[10];
    auto count = backtrace(buffer, 10);
    auto start = bit_cast<FlatPtr>(m_code);
    auto end = start + m_size;
    for (auto i = 0; i < count; i++) {
        auto address = bit_cast<FlatPtr>(buffer[i]);
        if (address < start || address >= end)
            continue;
        // return address points after the call
        // let's subtract 1 to make sure we don't hit the next bytecode
        // (in practice that's not necessary, because our native_call() sequence continues)
        auto offset = address - start - 1;
        auto& entry = find_mapping_entry(offset);
        if (entry.block_index < executable.basic_blocks.size()) {
            auto const& block = *executable.basic_blocks[entry.block_index];
            if (entry.bytecode_offset < block.size()) {
                // This is rather clunky, but Interpreter::instruction_stream_iterator() gives out references, so we need to keep it alive.
                m_instruction_stream_iterator = make<Bytecode::InstructionStreamIterator>(block.instruction_stream(), &executable, entry.bytecode_offset);
                return *m_instruction_stream_iterator;
            }
        }
    }
#endif
    return {};
}

}
