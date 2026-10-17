// jit.cpp - x64 code generator (Windows ABI).
//
// Register use inside generated code:
//   rbx  Ctx*                    r12  tape pointer (absolute)
//   r13  ctx->ram_base           rax/rcx/rdx scratch
//
// Invariant: whenever control is at a block entry, JZ/JNZ/SCAN or END, r12
// points into RAM.  Each straight-line block checks its lowest reachable
// address once; if that could fall into the device window the block (and
// whatever follows while the pointer is in the window) runs in the interpreter,
// which returns the native address at which to resume.
#include "internal.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <climits>
#include <algorithm>
#undef IN      // windows.h SAL macros clash with the IR opcode names
#undef OUT

namespace bf {

namespace {

struct Emitter {
    std::vector<uint8_t> c;
    size_t pos() const { return c.size(); }
    void b(std::initializer_list<int> bytes) { for (int x : bytes) c.push_back((uint8_t)x); }
    void d8(int v) { c.push_back((uint8_t)v); }
    void d32(int32_t v) { for (int i = 0; i < 4; i++) c.push_back((uint8_t)((uint32_t)v >> (8 * i))); }
    void patch32(size_t at, int32_t v) { for (int i = 0; i < 4; i++) c[at + i] = (uint8_t)((uint32_t)v >> (8 * i)); }
    static bool fits8(int64_t v) { return v >= -128 && v <= 127; }

    // ModRM + SIB for [r12 + disp] with the given reg field (REX.B set by caller).
    void r12mem(int reg, int32_t disp) {
        if (disp == 0)         { b({0x04 | (reg << 3), 0x24}); }
        else if (fits8(disp))  { b({0x44 | (reg << 3), 0x24}); d8(disp); }
        else                   { b({0x84 | (reg << 3), 0x24}); d32(disp); }
    }
    // Conditional / unconditional rel32 jumps; return the patch position.
    enum { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5 };
    size_t jcc(int cc)   { b({0x0F, 0x80 | cc}); size_t at = pos(); d32(0); return at; }
    size_t jmp()         { b({0xE9}); size_t at = pos(); d32(0); return at; }
    void   bind(size_t at, size_t target) { patch32(at, (int32_t)((int64_t)target - (int64_t)(at + 4))); }

    void add_r12(int32_t k) {
        if (fits8(k)) { b({0x49, 0x83, 0xC4}); d8(k); } else { b({0x49, 0x81, 0xC4}); d32(k); }
    }
    void cmp_r12_zero()        { b({0x41, 0x80, 0x3C, 0x24, 0x00}); }   // cmp byte [r12], 0
    void cmp_r12_r13()         { b({0x4D, 0x39, 0xEC}); }               // cmp r12, r13
    void mov_ctxptr_from_r12() { b({0x4C, 0x89, 0x63, 0x00}); }         // mov [rbx+0], r12
    void mov_r12_from_ctxptr() { b({0x4C, 0x8B, 0x63, 0x00}); }         // mov r12, [rbx+0]
    void mov_rcx_rbx()         { b({0x48, 0x89, 0xD9}); }
    void call_ctx(int disp8)   { b({0xFF, 0x53, disp8}); }              // call [rbx+disp8]

    // Hand off to the interpreter from IR index pc; it returns the resume address.
    void slow_path(int32_t pc) {
        mov_ctxptr_from_r12();
        mov_rcx_rbx();
        b({0xBA}); d32(pc);                 // mov edx, pc
        call_ctx(64);                       // slow_fn
        mov_r12_from_ctxptr();
        b({0xFF, 0xE0});                    // jmp rax
    }
};

struct Fixup { size_t at; int32_t target_pc; };

} // namespace

JitCode::~JitCode() { if (mem) VirtualFree(mem, 0, MEM_RELEASE); }

bool jit_compile(const Program& p, JitCode& out) {
    const auto& code = p.code;
    const size_t n = code.size();
    Emitter e;
    std::vector<size_t> label_pos(n, SIZE_MAX);
    std::vector<Fixup> fixups;
    std::vector<size_t> end_jumps;

    // prologue
    e.b({0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});   // push rbx r12 r13 r14 r15
    e.b({0x48, 0x83, 0xEC, 0x20});                                 // sub rsp, 32
    e.b({0x48, 0x89, 0xCB});                                       // mov rbx, rcx
    e.mov_r12_from_ctxptr();
    e.b({0x4C, 0x8B, 0x6B, 0x08});                                 // mov r13, [rbx+8]
    e.b({0x48, 0x89, 0x63, 0x10});                                 // mov [rbx+16], rsp

    size_t i = 0;
    while (i < n) {
        if (has_label(p, i)) label_pos[i] = e.pos();
        const Instr& x = code[i];
        switch (x.op) {
        case JZ:
            e.cmp_r12_zero();
            fixups.push_back({e.jcc(Emitter::CC_E), x.target});
            i++; break;
        case JNZ:
            e.cmp_r12_zero();
            fixups.push_back({e.jcc(Emitter::CC_NE), x.target});
            i++; break;
        case ENDIF:
            i++; break;
        case END:
            e.b({0x31, 0xC0});                                     // xor eax, eax
            end_jumps.push_back(e.jmp());
            i++; break;
        case SCAN: {
            size_t top = e.pos();
            e.cmp_r12_zero();
            size_t done = e.jcc(Emitter::CC_E);
            e.add_r12(x.arg);
            e.cmp_r12_r13();
            size_t back = e.jcc(Emitter::CC_AE);
            e.bind(back, top);
            e.slow_path((int32_t)i);                               // pointer reached the device window
            e.bind(done, e.pos());
            i++; break;
        }
        default: {
            // straight-line block [i, j)
            size_t j = i;
            int64_t minoff = 0, delta = 0;           // lowest address reached, relative to r12 at block entry
            while (j < n && !is_special(code[j].op)) {
                const Instr& y = code[j];
                if (y.op == MOVE) { delta += y.arg; minoff = std::min(minoff, delta); }
                else {
                    minoff = std::min(minoff, delta + y.off);
                    if (y.op == MUL) minoff = std::min(minoff, delta + y.target);
                }
                j++;
            }
            size_t guard = SIZE_MAX;
            if (minoff < 0) {
                if (minoff < INT32_MIN) { out.error = "block offset out of range"; return false; }
                e.b({0x49, 0x8D}); e.r12mem(0, (int32_t)minoff);  // lea rax, [r12+minoff]
                e.b({0x4C, 0x39, 0xE8});                           // cmp rax, r13
                guard = e.jcc(Emitter::CC_B);
            }
            int32_t cached_src = INT32_MIN;                        // offset currently held in ecx
            for (size_t k = i; k < j; k++) {
                const Instr& y = code[k];
                switch (y.op) {
                case ADD:
                    e.b({0x41, 0x80}); e.r12mem(0, y.off); e.d8(y.arg);            // add byte [r12+off], imm8
                    if (y.off == cached_src) cached_src = INT32_MIN;
                    break;
                case SET:
                    e.b({0x41, 0xC6}); e.r12mem(0, y.off); e.d8(y.arg);            // mov byte [r12+off], imm8
                    if (y.off == cached_src) cached_src = INT32_MIN;
                    break;
                case MUL: {
                    if (cached_src != y.target) {
                        e.b({0x41, 0x0F, 0xB6}); e.r12mem(1, y.target);            // movzx ecx, byte [r12+src]
                        cached_src = y.target;
                    }
                    uint8_t m = (uint8_t)y.arg;
                    if (m == 1)        { e.b({0x41, 0x00}); e.r12mem(1, y.off); }  // add [r12+off], cl
                    else if (m == 255) { e.b({0x41, 0x28}); e.r12mem(1, y.off); }  // sub [r12+off], cl
                    else {
                        e.b({0x6B, 0xC1}); e.d8((int8_t)m);                        // imul eax, ecx, imm8
                        e.b({0x41, 0x00}); e.r12mem(0, y.off);                     // add [r12+off], al
                    }
                    if (y.off == cached_src) cached_src = INT32_MIN;
                    break;
                }
                case MOVE:
                    e.add_r12(y.arg);
                    cached_src = INT32_MIN;
                    break;
                case OUT:
                    e.b({0x41, 0x0F, 0xB6}); e.r12mem(2, y.off);                   // movzx edx, byte [r12+off]
                    e.mov_rcx_rbx();
                    e.call_ctx(48);
                    cached_src = INT32_MIN;
                    break;
                case IN:
                    e.mov_rcx_rbx();
                    e.call_ctx(56);
                    e.b({0x3D}); e.d32(256);                                       // cmp eax, 256
                    e.b({0x73}); { size_t sk = e.pos(); e.d8(0);                   // jae skip
                    e.b({0x41, 0x88}); e.r12mem(0, y.off);                         // mov [r12+off], al
                    e.c[sk] = (uint8_t)(e.pos() - (sk + 1)); }
                    cached_src = INT32_MIN;
                    break;
                default: break;
                }
            }
            if (guard != SIZE_MAX) {
                size_t over = e.jmp();
                e.bind(guard, e.pos());
                e.slow_path((int32_t)i);
                e.bind(over, e.pos());
            }
            i = j;
        }
        }
    }

    // exits
    size_t exit_normal = e.pos();
    e.mov_ctxptr_from_r12();
    size_t exit_common = e.pos();
    e.b({0x89, 0x43, 0x20});                                       // mov [rbx+32], eax
    e.b({0x48, 0x83, 0xC4, 0x20});                                 // add rsp, 32
    e.b({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B});   // pop r15 r14 r13 r12 rbx
    e.b({0xC3});
    size_t exit_slow = e.pos();
    e.b({0x8B, 0x43, 0x20});                                       // mov eax, [rbx+32]
    size_t js = e.jmp(); e.bind(js, exit_normal);

    for (size_t at : end_jumps) e.bind(at, exit_normal);
    for (const Fixup& f : fixups) {
        if ((size_t)f.target_pc >= n || label_pos[f.target_pc] == SIZE_MAX) { out.error = "internal: jump to unlabelled IR"; return false; }
        e.bind(f.at, label_pos[f.target_pc]);
    }

    out.size = e.pos();
    out.mem = (uint8_t*)VirtualAlloc(nullptr, out.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!out.mem) { out.error = "VirtualAlloc failed"; return false; }
    memcpy(out.mem, e.c.data(), out.size);
    DWORD old;
    if (!VirtualProtect(out.mem, out.size, PAGE_EXECUTE_READ, &old)) { out.error = "VirtualProtect failed"; return false; }
    FlushInstructionCache(GetCurrentProcess(), out.mem, out.size);

    out.entry = (int32_t(*)(Ctx*))out.mem;
    out.exit_common = out.mem + exit_common;
    out.exit_slow = out.mem + exit_slow;
    out.labels.assign(n, nullptr);
    for (size_t k = 0; k < n; k++) if (label_pos[k] != SIZE_MAX) out.labels[k] = out.mem + label_pos[k];
    return true;
}

} // namespace bf
