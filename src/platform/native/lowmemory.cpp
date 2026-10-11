// The PS2's low memory for the retail code's null pointers. On the PS2 the game runs with the kernel's RAM at address 0: a read
// through a null pointer (or a small offset from one) returns kernel memory and goes on, a write changes it (docs/RETAIL_BUGS.md,
// "Low addresses"); several are reached in normal play. Nothing can be mapped there natively (macOS keeps the first 4 GB
// unmapped), so a load or store faulting below LowMemoryLimit is done here instead: the faulting instruction is decoded and
// carried out on g_NativeLowMemory, and the program goes on after it. A jump or call through such an address crashes, as on the
// PS2. g_NativeLowMemory holds zeros unless a dump of the PS2's (build/native/ps2_low_memory.bin) is given (native/NATIVE.md)
#include "native.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <sys/ucontext.h>
#include <unistd.h>

extern "C"
{
    alignas(16) u8 g_NativeLowMemory[NativeLowMemoryBytes] = {};
}

namespace
{
#if defined(__aarch64__)
constexpr uiptr LowMemoryLimit = NativeLowMemoryBytes;

struct sigaction g_Previous[2];
u32 g_Emulated = 0;

// The registers of the interrupted thread: x0-x30, sp, pc and the 32 vector registers
struct Registers
{
#if defined(__APPLE__)
    explicit Registers(ucontext_t* context) : state(&context->uc_mcontext->__ss), vectors(&context->uc_mcontext->__ns)
    {
    }

    u64& X(u32 index)
    {
        return index == 29 ? reinterpret_cast<u64&>(state->__fp) : index == 30 ? reinterpret_cast<u64&>(state->__lr)
                                                                              : reinterpret_cast<u64&>(state->__x[index]);
    }
    u64& Sp()
    {
        return reinterpret_cast<u64&>(state->__sp);
    }
    u64& Pc()
    {
        return reinterpret_cast<u64&>(state->__pc);
    }
    u8* Vector(u32 index)
    {
        return reinterpret_cast<u8*>(&vectors->__v[index]);
    }

    __darwin_arm_thread_state64* state;
    __darwin_arm_neon_state64* vectors;
#else
    explicit Registers(ucontext_t* context) : machine(&context->uc_mcontext)
    {
        // The vector registers are in the reserved area's FPSIMD record
        auto* record = reinterpret_cast<_aarch64_ctx*>(machine->__reserved);
        while (record->magic != 0 && record->magic != FPSIMD_MAGIC)
        {
            record = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<u8*>(record) + record->size);
        }

        simd = record->magic == FPSIMD_MAGIC ? reinterpret_cast<fpsimd_context*>(record) : nullptr;
    }

    u64& X(u32 index)
    {
        return reinterpret_cast<u64&>(machine->regs[index]);
    }
    u64& Sp()
    {
        return reinterpret_cast<u64&>(machine->sp);
    }
    u64& Pc()
    {
        return reinterpret_cast<u64&>(machine->pc);
    }
    u8* Vector(u32 index)
    {
        return simd != nullptr ? reinterpret_cast<u8*>(&simd->vregs[index]) : nullptr;
    }

    mcontext_t* machine;
    fpsimd_context* simd;
#endif
};

s64 SignExtend(u64 value, u32 bits)
{
    u64 sign = 1ull << (bits - 1);
    return static_cast<s64>((value ^ sign) - sign);
}

bool InLowMemory(u64 address, u32 bytes)
{
    return address < LowMemoryLimit && address + bytes <= LowMemoryLimit;
}

// One register's load or store at an address of low memory (size in bytes; signed loads extend to 32 or 64 bits)
bool Access(Registers& registers, bool vector, bool load, u32 size, bool signExtend, bool to32, u32 rt, u64 address)
{
    if (!InLowMemory(address, size))
    {
        return false;
    }

    u8* memory = g_NativeLowMemory + address;
    if (vector)
    {
        u8* reg = registers.Vector(rt);
        if (reg == nullptr)
        {
            return false;
        }

        if (load)
        {
            std::memset(reg, 0, 16);
            std::memcpy(reg, memory, size);
        }
        else
        {
            std::memcpy(memory, reg, size);
        }

        return true;
    }

    if (load)
    {
        u64 value = 0;
        std::memcpy(&value, memory, size);
        if (signExtend)
        {
            s64 extended = SignExtend(value, size * 8);
            value = to32 ? static_cast<u32>(extended) : static_cast<u64>(extended);
        }

        if (rt != 31)
        {
            registers.X(rt) = value;
        }
    }
    else
    {
        u64 value = rt == 31 ? 0 : registers.X(rt);
        std::memcpy(memory, &value, size);
    }

    return true;
}

u64 Base(Registers& registers, u32 rn)
{
    return rn == 31 ? registers.Sp() : registers.X(rn);
}

void WriteBase(Registers& registers, u32 rn, u64 value)
{
    if (rn == 31)
    {
        registers.Sp() = value;
    }
    else
    {
        registers.X(rn) = value;
    }
}

// The single register loads and stores' size and kind from size (bits 31-30), V (26) and opc (23-22)
bool SingleKind(u32 instruction, bool* vector, u32* bytes, bool* load, bool* signExtend, bool* to32)
{
    u32 size = instruction >> 30;
    u32 opc = (instruction >> 22) & 3;
    *vector = ((instruction >> 26) & 1) != 0;
    if (*vector)
    {
        u32 scale = ((opc & 2) << 1) | size;
        if (scale > 4)
        {
            return false;
        }

        *bytes = 1u << scale;
        *load = (opc & 1) != 0;
        *signExtend = false;
        *to32 = false;
        return true;
    }

    *bytes = 1u << size;
    switch (opc)
    {
    case 0:
        *load = false;
        *signExtend = false;
        *to32 = false;
        return true;
    case 1:
        *load = true;
        *signExtend = false;
        *to32 = false;
        return true;
    case 2:
        // A signed load to 64 bits (size 3: a prefetch, done by doing nothing)
        *load = true;
        *signExtend = size != 3;
        *to32 = false;
        return size != 3 || true;
    default:
        *load = true;
        *signExtend = true;
        *to32 = true;
        return size < 2;
    }
}

bool Emulate(Registers& registers)
{
    u32 instruction;
    std::memcpy(&instruction, reinterpret_cast<const void*>(registers.Pc()), sizeof(instruction));
    u32 rt = instruction & 31;
    u32 rn = (instruction >> 5) & 31;
    // A prefetch (PRFM, PRFUM: size 3, opc 2 of the single register forms) does nothing
    if ((instruction & 0x3B000000) == 0x39000000 || (instruction & 0x3B000000) == 0x38000000)
    {
        if (((instruction >> 26) & 1) == 0 && (instruction >> 30) == 3 && ((instruction >> 22) & 3) == 2)
        {
            return true;
        }
    }

    // Load/store register, unsigned immediate: x x 1 1 1 V 0 1 opc imm12 Rn Rt
    if ((instruction & 0x3B000000) == 0x39000000)
    {
        bool vector, load, signExtend, to32;
        u32 bytes;
        if (!SingleKind(instruction, &vector, &bytes, &load, &signExtend, &to32))
        {
            return false;
        }

        if (!vector && (instruction >> 30) == 3 && ((instruction >> 22) & 3) == 2)
        {
            return true;
        }

        u64 address = Base(registers, rn) + ((instruction >> 10) & 0xFFF) * bytes;
        return Access(registers, vector, load, bytes, signExtend, to32, rt, address);
    }

    // Load/store register, unscaled, post- and pre-indexed, register offset: x x 1 1 1 V 0 0 opc ...
    if ((instruction & 0x3B000000) == 0x38000000)
    {
        bool vector, load, signExtend, to32;
        u32 bytes;
        if (!SingleKind(instruction, &vector, &bytes, &load, &signExtend, &to32))
        {
            return false;
        }

        u32 mode = (instruction >> 10) & 3;
        u64 base = Base(registers, rn);
        if (((instruction >> 21) & 1) != 0)
        {
            if (mode != 2)
            {
                return false;
            }

            // Register offset: Rm extended by option, shifted by the size when S
            u32 rm = (instruction >> 16) & 31;
            u32 option = (instruction >> 13) & 7;
            u64 offset = rm == 31 ? 0 : registers.X(rm);
            switch (option)
            {
            case 2:
                offset = static_cast<u32>(offset);
                break;
            case 6:
                offset = static_cast<u64>(static_cast<s64>(static_cast<s32>(offset)));
                break;
            default:
                break;
            }

            if (((instruction >> 12) & 1) != 0)
            {
                offset <<= __builtin_ctz(bytes);
            }

            return Access(registers, vector, load, bytes, signExtend, to32, rt, base + offset);
        }

        s64 offset = SignExtend((instruction >> 12) & 0x1FF, 9);
        u64 address = mode == 1 ? base : base + offset;
        if (!Access(registers, vector, load, bytes, signExtend, to32, rt, address))
        {
            return false;
        }

        if (mode == 1 || mode == 3)
        {
            WriteBase(registers, rn, base + offset);
        }

        return true;
    }

    // Load/store pair: opc x 1 0 1 V 0 mode L imm7 Rt2 Rn Rt (mode 01 post, 10 offset, 11 pre)
    if ((instruction & 0x3A000000) == 0x28000000)
    {
        u32 opc = instruction >> 30;
        bool vector = ((instruction >> 26) & 1) != 0;
        bool load = ((instruction >> 22) & 1) != 0;
        u32 mode = (instruction >> 23) & 3;
        u32 rt2 = (instruction >> 10) & 31;
        u32 bytes = vector ? 4u << opc : opc == 2 ? 8 : 4;
        bool signExtend = !vector && opc == 1;
        if (mode == 0)
        {
            return false;
        }

        u64 base = Base(registers, rn);
        s64 offset = SignExtend((instruction >> 15) & 0x7F, 7) * bytes;
        u64 address = mode == 1 ? base : base + offset;
        if (!Access(registers, vector, load, bytes, signExtend, false, rt, address) ||
            !Access(registers, vector, load, bytes, signExtend, false, rt2, address + bytes))
        {
            return false;
        }

        if (mode == 1 || mode == 3)
        {
            WriteBase(registers, rn, base + offset);
        }

        return true;
    }

    return false;
}

void OnFault(int signal, siginfo_t* info, void* context)
{
    auto address = reinterpret_cast<uiptr>(info->si_addr);
    Registers registers(static_cast<ucontext_t*>(context));
    if (address < LowMemoryLimit && registers.Pc() != address && Emulate(registers))
    {
        registers.Pc() += 4;
        if (g_Emulated++ == 0 && Native::GetSettings().logStubs)
        {
            // Once: the PS2's way with its null pointers is being followed (see native/NATIVE.md)
            static const char message[] = "[native] a retail null pointer read/write went to the PS2's low memory\n";
            ::write(2, message, sizeof(message) - 1);
        }

        return;
    }

    // Anything else: the fault it is (the previous handler, else the default, which ends the program), said first with where it
    // happened, since the default ends the program without a word
    char message[128];
    int length = std::snprintf(message, sizeof(message), "[native] fatal %s at %#llx (pc %#llx), backtrace:\n",
                               signal == SIGBUS ? "SIGBUS" : "SIGSEGV", static_cast<unsigned long long>(address),
                               static_cast<unsigned long long>(registers.Pc()));
    ::write(2, message, static_cast<size_t>(length));
#if defined(__APPLE__)
    const auto& state = static_cast<ucontext_t*>(context)->uc_mcontext->__ss;
    for (int i = 0; i < 29; i += 4)
    {
        length = std::snprintf(message, sizeof(message), "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i,
                               static_cast<unsigned long long>(state.__x[i]), i + 1,
                               static_cast<unsigned long long>(i + 1 < 29 ? state.__x[i + 1] : state.__fp), i + 2,
                               static_cast<unsigned long long>(i + 2 < 29 ? state.__x[i + 2] : state.__lr), i + 3,
                               static_cast<unsigned long long>(i + 3 < 29 ? state.__x[i + 3] : state.__sp));
        ::write(2, message, static_cast<size_t>(length));
    }
#endif
    void* frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
    struct sigaction& previous = g_Previous[signal == SIGBUS ? 1 : 0];
    sigaction(signal, &previous, nullptr);
}
#endif
}

namespace Native
{
// Loads the PS2's low memory dump when there's one, and starts emulating the retail code's low accesses
void StartLowMemory()
{
    std::ifstream dump("build/native/ps2_low_memory.bin", std::ios::binary);
    if (dump)
    {
        dump.read(reinterpret_cast<char*>(g_NativeLowMemory), NativeLowMemoryBytes);
        Log("the PS2's low memory from build/native/ps2_low_memory.bin");
    }

#if defined(__aarch64__)
    struct sigaction action = {};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &g_Previous[0]);
    sigaction(SIGBUS, &action, &g_Previous[1]);
#else
    Log("the PS2's low memory isn't emulated on this machine's processor: a retail null read will crash");
#endif
}
}

// A free of memory that isn't the game's pool (memory.cpp): where it came from, once a place
void NativeBadFree(void* memory, const void* pages, const void* pagesEnd)
{
    static int reported = 0;
    if (reported++ < 8)
    {
        char message[128];
        int length = std::snprintf(message, sizeof(message), "[native] a free of %p, outside the small pages %p-%p, left alone:\n", memory, pages, pagesEnd);
        ::write(2, message, static_cast<size_t>(length));
        void* frames[24];
        backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
    }
}
