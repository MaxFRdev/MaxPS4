#pragma once
// Translates a GCN vertex or pixel program, read from guest memory, into
// one portable shader function. The same text compiles as C++ against
// GCNShaderRuntime.hpp (tests, compared with GCNInterpreter.hpp) and as
// Metal against gcnMetalPrelude(). Control flow becomes a block dispatcher
// (a switch in a loop), so any branch structure translates. A fetch shader
// reached with s_swappc_b64 through user SGPRs is translated in place; its
// s_setpc_b64 returns to the calling block.
//
// Execution model (see GCNShaderRuntime.hpp): one lane per invocation with
// local scalar registers; lane masks keep this lane's bit in bit 0; branches
// on VCC/EXEC, WQM and SCC of 64-bit mask operations are decided per 2x2
// quad. Not translated, reported as errors: LDS (other than ds_swizzle in
// quad mode), memory stores outside compute programs, lane counts of masks,
// s_getpc. A compute program runs one lane per thread: user SGPRs, then the
// enabled thread group ids, in L.user; thread ids in v0-v2.
#include "GCNDecoder.hpp"
#include "GCNInterpreter.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace maxps4::gpu {
enum class GCNStage { vertex, pixel, compute };
struct GCNTranslateOptions {
    GCNStage stage = GCNStage::pixel;
    std::uint32_t userData[16] = {}; // resolves s_swappc_b64 targets
    unsigned userCount = 0;
    std::string name = "gcn_program";
};
struct GCNTranslation {
    std::string error;           // empty on success
    std::string source;          // GCN_FN void <name>(GCNLane&, GCNMemory, GCNQuad&)
    std::uint32_t params = 0;    // vertex: parameters exported (bit N)
    std::uint32_t attributes = 0, flat = 0; // pixel: attributes read, of which flat
    std::uint8_t colors = 0;     // pixel: MRTs exported
    bool position = false, quad = false, mayKill = false;
    bool stores = false;         // compute: writes guest memory
    explicit operator bool() const { return error.empty(); }
};

class GCNTranslator {
public:
    GCNTranslator(const GCNMemory& memory, const GCNTranslateOptions& options) : memory_(memory), options_(options) {}
    GCNTranslation translate(std::uint64_t entry) {
        if (explore(entry)) {
            // A Sony prologue may precede the initial EXEC save with scalar
            // moves (typically the OrbShdr offset in vcc_hi) or NOPs.
            for (auto pc=entry;;) {
                const auto found=code_.find(pc);
                if (found==code_.end()) break;
                const auto& i=found->second.i;
                const unsigned d=(i.word0>>16)&127;
                if (i.encoding==GCNEncoding::sop1 && i.opcode==4 && (i.word0&255)==126 && d<103) {
                    savedEntryExec_=d; break;
                }
                if (!((i.encoding==GCNEncoding::sop1 && i.opcode==3 && d<126) ||
                      (i.encoding==GCNEncoding::sopp && i.opcode==0))) break;
                pc+=4*i.size;
            }
            emit(entry);
            out_.mayKill=execMayKill_ || (restoresEntryExec_ &&
                (scalarWrites_[savedEntryExec_]!=1 || scalarWrites_[savedEntryExec_+1]!=1));
        }
        if (!out_.error.empty()) out_.source.clear();
        return out_;
    }

private:
    struct Inst { GCNInstruction i; std::string name; };
    const GCNMemory& memory_;
    GCNTranslateOptions options_;
    GCNTranslation out_;
    std::map<std::uint64_t, Inst> code_;
    std::set<std::uint64_t> leaders_;
    std::map<std::uint64_t, std::uint64_t> calls_; // s_swappc address -> target
    std::set<unsigned> sregs_, vregs_;
    std::string body_;
    std::map<unsigned,unsigned> scalarWrites_;
    unsigned savedEntryExec_=128;
    bool safeExecWrite_=false, restoresEntryExec_=false, execMayKill_=false;

    bool fail(const std::string& message) { if (out_.error.empty()) out_.error = message; return false; }
    static std::string hex(std::uint64_t v, const char* suffix = "u") {
        char b[32]; std::snprintf(b, sizeof b, "0x%llxu", (unsigned long long)v);
        std::string s = b; s.pop_back(); return s + suffix;
    }
    static std::uint64_t branchTarget(std::uint64_t pc, const GCNInstruction& i) {
        return pc + 4 + 4 * std::int64_t(std::int16_t(i.word0 & 0xffff));
    }

    // ---- control flow discovery ----
    bool decodeAt(std::uint64_t pc, Inst& in) {
        std::uint32_t words[3] = {};
        const auto available = readGCNWords(memory_, pc, words);
        if (!available) return fail("unreadable code");
        if (decodeGCNInstruction(words, available, std::size_t(pc / 4), in.i) != GCNError::none) return fail("undecodable code");
        in.name = gcnMnemonic(in.i);
        return true;
    }
    bool explore(std::uint64_t entry) {
        std::vector<std::uint64_t> work{entry};
        leaders_.insert(entry);
        while (!work.empty()) {
            std::uint64_t pc = work.back();
            work.pop_back();
            while (!code_.count(pc)) {
                if (code_.size() > 16384) return fail("program too large");
                Inst in;
                if (!decodeAt(pc, in)) return false;
                code_[pc] = in;
                const GCNInstruction& i = in.i;
                const std::uint64_t next = pc + 4ull * i.size;
                if (i.encoding == GCNEncoding::sopp) {
                    if (i.opcode == 1) break;
                    if (i.opcode == 2) { const auto t = branchTarget(pc, i); leaders_.insert(t); work.push_back(t); break; }
                    if (i.opcode >= 4 && i.opcode <= 9) {
                        const auto t = branchTarget(pc, i); leaders_.insert(t); work.push_back(t); leaders_.insert(next);
                    }
                }
                if (i.encoding == GCNEncoding::sop1 && i.opcode == 33) { // s_swappc_b64
                    const unsigned src = i.word0 & 0xff;
                    if (src + 1 >= options_.userCount) return fail("s_swappc_b64 target not in user SGPRs");
                    const std::uint64_t t = std::uint64_t(options_.userData[src]) | std::uint64_t(options_.userData[src + 1]) << 32;
                    calls_[pc] = t; leaders_.insert(t); work.push_back(t); leaders_.insert(next); work.push_back(next);
                    break;
                }
                if (i.encoding == GCNEncoding::sop1 && i.opcode == 32) break; // s_setpc_b64
                pc = next;
            }
        }
        return true;
    }
    std::size_t index(std::uint64_t address) const { return std::size_t(std::distance(leaders_.begin(), leaders_.find(address))); }

    // ---- operands ----
    std::string s(unsigned n) { sregs_.insert(n); return "s" + std::to_string(n); }
    std::string v(unsigned n) { vregs_.insert(n & 0xff); return "v" + std::to_string(n & 0xff); }
    // A scalar or constant source, as a uint expression.
    std::string scalar(unsigned code, const GCNInstruction& i) {
        if (code <= 103 || (code >= 106 && code <= 127 && code != 125)) return s(code);
        if (code == 128) return "0u";
        if (code <= 192) return std::to_string(code - 128) + "u";
        if (code <= 208) return hex(std::uint32_t(192 - int(code)));
        static const std::uint32_t floats[] = {0x3f000000, 0xbf000000, 0x3f800000, 0xbf800000,
                                               0x40000000, 0xc0000000, 0x40800000, 0xc0800000};
        if (code >= 240 && code <= 247) return hex(floats[code - 240]);
        if (code == 253) return "(scc ? 1u : 0u)";
        if (code == 255) return hex(i.literal);
        fail("unsupported scalar operand " + std::to_string(code));
        return "0u";
    }
    std::string scalar64(unsigned code, const GCNInstruction& i) {
        if (code <= 103 || (code >= 106 && code <= 126 && code != 125)) return "gcn_u64(" + s(code) + ", " + s(code + 1) + ")";
        if (code >= 193 && code <= 208) return hex(std::uint64_t(std::int64_t(192 - int(code))), "ul");
        return "ulong(" + scalar(code, i) + ")";
    }
    std::string source(unsigned code, const GCNInstruction& i) { return code >= 256 ? v(code - 256) : scalar(code, i); }
    void setScalar(unsigned d, const std::string& value) {
        if (d > 127) { fail("unsupported scalar destination"); return; }
        ++scalarWrites_[d];
        if (d>=126 && !safeExecWrite_) execMayKill_=true;
        body_ += s(d) + " = " + value + "; ";
    }
    void setScalar64(unsigned d, const std::string& value) {
        if (d > 126) { fail("unsupported scalar destination"); return; }
        ++scalarWrites_[d]; ++scalarWrites_[d+1];
        if (d>=125 && !safeExecWrite_) execMayKill_=true;
        body_ += "{ ulong w = " + value + "; " + s(d) + " = uint(w); " + s(d + 1) + " = uint(w >> 32); } ";
    }
    static const char* ex() { return "((s126 & 1u) != 0u)"; }
    // A wave-level decision: per quad in pixel programs, per lane otherwise
    // (Metal has no quad operations in vertex functions).
    std::string any(const std::string& cond) {
        if (options_.stage != GCNStage::pixel) return "(" + cond + ")";
        out_.quad = true;
        return "gcn_quad_any(Q, L, " + cond + ")";
    }

    // ---- emission ----
    void emit(std::uint64_t entry) {
        std::size_t k = 0;
        for (const auto leader : leaders_) {
            body_ += "    case " + std::to_string(k++) + "u: {\n";
            std::uint64_t pc = leader;
            for (;;) {
                const auto found = code_.find(pc);
                if (found == code_.end()) { fail("block falls into unexplored code"); return; }
                body_ += "        ";
                bool ended = false;
                if (!instruction(found->second, pc, ended)) {
                    if (out_.error.empty()) fail("unsupported " + found->second.name);
                    char at[32]; std::snprintf(at, sizeof at, " at 0x%llx", (unsigned long long)pc);
                    out_.error += at;
                    return;
                }
                body_ += "\n";
                if (ended) break;
                const std::uint64_t next = pc + 4ull * found->second.i.size;
                if (leaders_.count(next)) { body_ += "        pc = " + std::to_string(index(next)) + "u; break;\n"; break; }
                pc = next;
            }
            body_ += "    }\n";
        }
        std::string head = "GCN_FN void " + options_.name + "(GCN_THREAD GCNLane& L, GCNMemory M, GCN_THREAD GCNQuad& Q) {\n";
        for (const auto n : sregs_) {
            std::string init = "0u";
            if (n < 20) init = "L.user[" + std::to_string(n) + "]";
            if (n == 126) init = "(L.exec ? 1u : 0u)";
            head += "    uint s" + std::to_string(n) + " = " + init + ";\n";
        }
        for (const auto n : vregs_)
            head += "    uint v" + std::to_string(n) + " = " + (n < 24 ? "L.vinit[" + std::to_string(n) + "]" : std::string("0u")) + ";\n";
        // Blocks are numbered by address: a fetch shader below the program
        // comes first, so execution starts at the entry's own block.
        head += "    bool scc = false; uint pc = " + std::to_string(index(entry)) + "u; bool running = true;\n"
                "    (void)scc; (void)M; (void)Q;\n"
                "    while (running) {\n    switch (pc) {\n";
        out_.source = head + body_ + "    default: running = false; break;\n    }\n    }\n}\n";
    }

    bool instruction(const Inst& in, std::uint64_t pc, bool& ended) {
        const GCNInstruction& i = in.i;
        switch (i.encoding) {
        case GCNEncoding::sop2: return sop2(in);
        case GCNEncoding::sopk: return sopk(in);
        case GCNEncoding::sop1: return sop1(in, pc, ended);
        case GCNEncoding::sopc: return sopc(in);
        case GCNEncoding::sopp: return sopp(in, pc, ended);
        case GCNEncoding::smrd: return smrd(in);
        case GCNEncoding::vop1: case GCNEncoding::vop2: case GCNEncoding::vopc: case GCNEncoding::vop3: return vector(in);
        case GCNEncoding::vintrp: return vintrp(in);
        case GCNEncoding::ds: return ds(in);
        case GCNEncoding::mubuf: return mubuf(in);
        case GCNEncoding::mimg: return mimg(in);
        case GCNEncoding::exp: return exportOp(in);
        default: return false;
        }
    }

    bool sop2(const Inst& in) {
        const GCNInstruction& i = in.i;
        const unsigned d = (i.word0 >> 16) & 0x7f, a = i.word0 & 0xff, b = (i.word0 >> 8) & 0xff;
        const std::string x = scalar(a, i), y = scalar(b, i);
        body_ += "{ uint x = " + x + "; uint y = " + y + "; (void)x; (void)y; ";
        auto r32 = [&](const std::string& e) { body_ += "uint r = " + e + "; "; setScalar(d, "r"); body_ += "scc = r != 0u; "; };
        auto r64 = [&](const std::string& op) {
            body_ += "ulong X = " + scalar64(a, i) + "; ulong Y = " + scalar64(b, i) + "; ulong r = " + op + "; ";
            setScalar64(d, "r");
            body_ += "scc = " + any("r != 0ul") + "; ";
        };
        switch (i.opcode) {
        case 0: body_ += "ulong t = ulong(x) + ulong(y); "; setScalar(d, "uint(t)"); body_ += "scc = (t >> 32) != 0ul; "; break;
        case 1: setScalar(d, "x - y"); body_ += "scc = y > x; "; break;
        case 2: body_ += "uint r = x + y; "; setScalar(d, "r"); body_ += "scc = ((~(x ^ y) & (x ^ r)) >> 31) != 0u; "; break;
        case 3: body_ += "uint r = x - y; "; setScalar(d, "r"); body_ += "scc = (((x ^ y) & (x ^ r)) >> 31) != 0u; "; break;
        case 4: body_ += "ulong t = ulong(x) + ulong(y) + (scc ? 1ul : 0ul); "; setScalar(d, "uint(t)"); body_ += "scc = (t >> 32) != 0ul; "; break;
        case 5: body_ += "ulong sub = ulong(y) + (scc ? 1ul : 0ul); "; setScalar(d, "uint(ulong(x) - sub)"); body_ += "scc = sub > ulong(x); "; break;
        case 6: body_ += "scc = int(x) < int(y); "; setScalar(d, "scc ? x : y"); break;
        case 7: body_ += "scc = x < y; "; setScalar(d, "scc ? x : y"); break;
        case 8: body_ += "scc = int(x) > int(y); "; setScalar(d, "scc ? x : y"); break;
        case 9: body_ += "scc = x > y; "; setScalar(d, "scc ? x : y"); break;
        case 10: setScalar(d, "scc ? x : y"); break;
        case 11: setScalar64(d, "scc ? " + scalar64(a, i) + " : " + scalar64(b, i)); break;
        case 14: r32("x & y"); break;  case 15: r64("X & Y"); break;
        case 16: r32("x | y"); break;  case 17: r64("X | Y"); break;
        case 18: r32("x ^ y"); break;  case 19: r64("X ^ Y"); break;
        case 20: r32("x & ~y"); break; case 21: r64("X & ~Y"); break;
        case 22: r32("x | ~y"); break; case 23: r64("X | ~Y"); break;
        case 24: r32("~(x & y)"); break; case 25: r64("~(X & Y)"); break;
        case 26: r32("~(x | y)"); break; case 27: r64("~(X | Y)"); break;
        case 28: r32("~(x ^ y)"); break; case 29: r64("~(X ^ Y)"); break;
        case 30: r32("x << (y & 31u)"); break;
        case 32: r32("x >> (y & 31u)"); break;
        case 34: r32("uint(int(x) >> (y & 31u))"); break;
        case 36: setScalar(d, "((1u << (x & 31u)) - 1u) << (y & 31u)"); break;
        case 38: setScalar(d, "x * y"); break;
        case 39: r32("((y >> 16) & 0x7fu) == 0u ? 0u : ((y >> 16) & 0x7fu) >= 32u ? (x >> (y & 31u)) : ((x >> (y & 31u)) & ((1u << ((y >> 16) & 0x7fu)) - 1u))"); break;
        default: return false;
        }
        body_ += "}";
        return out_.error.empty();
    }
    bool sopk(const Inst& in) {
        const GCNInstruction& i = in.i;
        const unsigned d = (i.word0 >> 16) & 0x7f;
        const std::uint32_t imm = i.word0 & 0xffff, simm = std::uint32_t(std::int32_t(std::int16_t(imm)));
        static const char* const ops[] = {"==", "!=", ">", ">=", "<", "<="};
        switch (i.opcode) {
        case 0: setScalar(d, hex(simm)); break;
        case 2: body_ += "if (scc) "; setScalar(d, hex(simm)); break;
        case 3: case 4: case 5: case 6: case 7: case 8:
            body_ += "scc = int(" + s(d) + ") " + ops[i.opcode - 3] + " int(" + hex(simm) + "); "; break;
        case 9: case 10: case 11: case 12: case 13: case 14:
            body_ += "scc = " + s(d) + " " + ops[i.opcode - 9] + " " + hex(imm) + "; "; break;
        case 15: setScalar(d, s(d) + " + " + hex(simm)); break;
        case 16: setScalar(d, s(d) + " * " + hex(simm)); break;
        case 18: setScalar(d, "0u"); break;
        case 19: case 21: break; // s_setreg: modes are not modelled
        default: return false;
        }
        return out_.error.empty();
    }
    bool sop1(const Inst& in, std::uint64_t pc, bool& ended) {
        const GCNInstruction& i = in.i;
        const unsigned d = (i.word0 >> 16) & 0x7f, a = i.word0 & 0xff;
        auto saveexec = [&](const std::string& op) {
            execMayKill_=true;
            body_ += "{ ulong X = " + scalar64(a, i) + "; ulong E = gcn_u64(s126, s127); ulong r = " + op + "; ";
            setScalar64(d, "E");
            body_ += s(126) + " = uint(r); " + s(127) + " = uint(r >> 32); scc = " + any("r != 0ul") + "; }";
        };
        switch (i.opcode) {
        case 3: setScalar(d, scalar(a, i)); break;
        case 4:
            safeExecWrite_=d==126 && a==savedEntryExec_;
            restoresEntryExec_|=safeExecWrite_;
            setScalar64(d, scalar64(a, i)); safeExecWrite_=false; break;
        case 5: body_ += "if (scc) "; setScalar(d, scalar(a, i)); break;
        case 6: body_ += "if (scc) "; setScalar64(d, scalar64(a, i)); break;
        case 7: body_ += "{ uint r = ~" + scalar(a, i) + "; "; setScalar(d, "r"); body_ += "scc = r != 0u; }"; break;
        case 8: body_ += "{ ulong r = ~" + scalar64(a, i) + "; "; setScalar64(d, "r"); body_ += "scc = " + any("r != 0ul") + "; }"; break;
        case 10: // s_wqm_b64: this lane is on if any lane of its quad is
            body_ += "{ bool on = " + any(scalar64(a, i) + " != 0ul") + "; ";
            safeExecWrite_=d==126 && a==126;
            setScalar64(d, "on ? 1ul : 0ul"); safeExecWrite_=false;
            body_ += "scc = on; }"; break;
        case 11: setScalar(d, "gcn_bfrev(" + scalar(a, i) + ")"); break;
        case 15: body_ += "{ uint r = gcn_popcount(" + scalar(a, i) + "); "; setScalar(d, "r"); body_ += "scc = r != 0u; }"; break;
        case 19: setScalar(d, "gcn_ffbl(" + scalar(a, i) + ")"); break;
        case 21: setScalar(d, "gcn_ffbh(" + scalar(a, i) + ")"); break;
        case 25: setScalar(d, "uint(int(" + scalar(a, i) + " << 24) >> 24)"); break;
        case 26: setScalar(d, "uint(int(" + scalar(a, i) + " << 16) >> 16)"); break;
        case 27: setScalar(d, s(d) + " & ~(1u << (" + scalar(a, i) + " & 31u))"); break;
        case 29: setScalar(d, s(d) + " | (1u << (" + scalar(a, i) + " & 31u))"); break;
        case 32: // s_setpc_b64: returns to the block index s_swappc_b64 stored
            body_ += "pc = " + s(a) + "; break;"; ended = true; break;
        case 33: {
            const auto call = calls_.find(pc);
            if (call == calls_.end()) return false;
            setScalar(d, std::to_string(index(pc + 4ull * i.size)) + "u");
            setScalar(d + 1, "0u");
            body_ += "pc = " + std::to_string(index(call->second)) + "u; break;";
            ended = true; break;
        }
        case 36: saveexec("X & E"); break;
        case 37: saveexec("X | E"); break;
        case 38: saveexec("X ^ E"); break;
        case 39: saveexec("X & ~E"); break;
        case 40: saveexec("X | ~E"); break;
        case 52: body_ += "{ int t = int(" + scalar(a, i) + "); uint r = uint(t < 0 ? -t : t); "; setScalar(d, "r"); body_ += "scc = r != 0u; }"; break;
        default: return false;
        }
        return out_.error.empty();
    }
    bool sopc(const Inst& in) {
        const GCNInstruction& i = in.i;
        const std::string x = scalar(i.word0 & 0xff, i), y = scalar((i.word0 >> 8) & 0xff, i);
        static const char* const ops[] = {"==", "!=", ">", ">=", "<", "<="};
        if (i.opcode < 6) body_ += "scc = int(" + x + ") " + ops[i.opcode] + " int(" + y + ");";
        else if (i.opcode < 12) body_ += "scc = " + x + " " + ops[i.opcode - 6] + " " + y + ";";
        else if (i.opcode == 12) body_ += "scc = ((" + x + " >> (" + y + " & 31u)) & 1u) == 0u;";
        else if (i.opcode == 13) body_ += "scc = ((" + x + " >> (" + y + " & 31u)) & 1u) != 0u;";
        else return false;
        return out_.error.empty();
    }
    bool sopp(const Inst& in, std::uint64_t pc, bool& ended) {
        const GCNInstruction& i = in.i;
        const std::uint64_t next = pc + 4ull * i.size;
        auto branch = [&](const std::string& cond) {
            body_ += "pc = (" + cond + ") ? " + std::to_string(index(branchTarget(pc, i))) + "u : " +
                     std::to_string(index(next)) + "u; break;";
            ended = true;
        };
        switch (i.opcode) {
        case 0: case 10: case 12: case 13: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 22:
            body_ += "/* " + in.name + " */"; return true;
        case 1: body_ += "running = false; break;"; ended = true; return true;
        case 2: body_ += "pc = " + std::to_string(index(branchTarget(pc, i))) + "u; break;"; ended = true; return true;
        case 4: branch("!scc"); return true;
        case 5: branch("scc"); return true;
        case 6: branch("!" + any(s(106) + " != 0u || " + s(107) + " != 0u")); return true;
        case 7: branch(any(s(106) + " != 0u || " + s(107) + " != 0u")); return true;
        case 8: branch("!" + any(s(126) + " != 0u || " + s(127) + " != 0u")); return true;
        case 9: branch(any(s(126) + " != 0u || " + s(127) + " != 0u")); return true;
        default: return false;
        }
    }
    bool smrd(const Inst& in) {
        const GCNInstruction& i = in.i;
        const unsigned d = (i.word0 >> 15) & 0x7f, base = ((i.word0 >> 9) & 0x3f) * 2;
        if (i.opcode == 30) { setScalar64(d, "0ul"); return out_.error.empty(); }
        if (i.opcode == 29 || i.opcode == 31) { body_ += "/* cache invalidation */"; return true; }
        if ((i.opcode & 7) > 4 || i.opcode > 12) return false;
        static const unsigned counts[] = {1, 2, 4, 8, 16};
        const unsigned count = counts[i.opcode & 7];
        std::string offset;
        if (i.word0 & 0x100) offset = hex(4ull * (i.word0 & 0xff), "ul");
        else if (i.hasLiteral) offset = hex(4ull * i.literal, "ul");
        else offset = "ulong(" + scalar(i.word0 & 0xff, i) + ")";
        // Several dwords are read in a loop after one gcn_locate of the whole
        // span: unrolled, a 16-dword load in copy mode (a range search per
        // dword) made Metal's vertex stage rasterize nothing (Mortal Kombat:
        // Legacy Kollection's matrix VS), while the same function was correct
        // without rasterization.
        const std::string n = std::to_string(count);
        if (i.opcode >= 8) {
            body_ += "{ ulong o = " + offset + "; ulong a = gcn_u64(" + s(base) + ", " + s(base + 1) + " & 0xffffu) + o; "
                     "uint stride = (" + s(base + 1) + " >> 16) & 0x3fffu; "
                     "ulong lim = stride != 0u ? ulong(" + s(base + 2) + ") * stride : ulong(" + s(base + 2) + "); ";
            if (count == 1) setScalar(d, "(o + 4ul <= lim) ? gcn_ld32(M, a) : 0u");
            else {
                body_ += "uint w[" + n + "]; ulong at = 0ul; "
                         "if (o + " + std::to_string(4 * count) + "ul <= lim && gcn_locate(M, a, " + std::to_string(4 * count) + "ul, at)) "
                         "{ for (uint k = 0u; k < " + n + "u; ++k) w[k] = gcn_packed32(M, at + 4ul * k); } "
                         "else { for (uint k = 0u; k < " + n + "u; ++k) w[k] = (o + 4ul * k + 4ul <= lim) ? gcn_ld32(M, a + 4ul * k) : 0u; } ";
                for (unsigned k = 0; k < count; ++k) setScalar(d + k, "w[" + std::to_string(k) + "]");
            }
            body_ += "}";
        } else {
            body_ += "{ ulong a = (gcn_u64(" + s(base) + ", " + s(base + 1) + ") & 0xfffffffffffful) + " + offset + "; ";
            if (count == 1) setScalar(d, "gcn_ld32(M, a)");
            else {
                body_ += "uint w[" + n + "]; ulong at = 0ul; "
                         "if (gcn_locate(M, a, " + std::to_string(4 * count) + "ul, at)) { for (uint k = 0u; k < " + n + "u; ++k) w[k] = gcn_packed32(M, at + 4ul * k); } "
                         "else { for (uint k = 0u; k < " + n + "u; ++k) w[k] = gcn_ld32(M, a + 4ul * k); } ";
                for (unsigned k = 0; k < count; ++k) setScalar(d + k, "w[" + std::to_string(k) + "]");
            }
            body_ += "}";
        }
        return out_.error.empty();
    }

    // Vector ALU, normalized to VOP3 opcode numbers as in the interpreter.
    bool vector(const Inst& in) {
        const GCNInstruction& i = in.i;
        unsigned op, dst, src[3] = {0, 0, 0};
        unsigned abs = 0, neg = 0, omod = 0, sdst = 106;
        bool clamp = false, vop3 = false;
        const std::uint32_t w = i.word0;
        switch (i.encoding) {
        case GCNEncoding::vop2: op = 256 + i.opcode; dst = (w >> 17) & 0xff; src[0] = w & 0x1ff; src[1] = 256 + ((w >> 9) & 0xff); break;
        case GCNEncoding::vop1: op = 384 + i.opcode; dst = (w >> 17) & 0xff; src[0] = w & 0x1ff; break;
        case GCNEncoding::vopc: op = i.opcode; dst = 0; src[0] = w & 0x1ff; src[1] = 256 + ((w >> 9) & 0xff); break;
        default:
            vop3 = true; op = i.opcode; dst = w & 0xff;
            src[0] = i.word1 & 0x1ff; src[1] = (i.word1 >> 9) & 0x1ff; src[2] = (i.word1 >> 18) & 0x1ff;
            omod = (i.word1 >> 27) & 3; neg = i.word1 >> 29;
            if ((op >= 293 && op <= 298) || op == 365 || op == 366 || op == 374 || op == 375) sdst = (w >> 8) & 0x7f;
            else { abs = (w >> 8) & 7; clamp = (w >> 11) & 1; }
            if (op < 256) sdst = dst;
            break;
        }
        const std::string& name = in.name;
        if (name.empty()) return false;
        const bool floatIn = gcn_detail::endsWith(name, "_f32");
        const bool floatOut = (floatIn && name.compare(0, 5, "v_cmp") != 0 && name != "v_cvt_u32_f32" &&
                               name != "v_cvt_i32_f32" && name != "v_cvt_pkrtz_f16_f32" && name != "v_cvt_flr_i32_f32" &&
                               name != "v_cvt_rpi_i32_f32" && name != "v_frexp_exp_i32_f32" && name != "v_cvt_f16_f32") ||
                              name.compare(0, 10, "v_cvt_f32_") == 0;
        auto operand = [&](unsigned n) {
            std::string x = source(src[n], i);
            if (floatIn && ((abs >> n) & 1)) x = "(" + x + " & 0x7fffffffu)";
            if (floatIn && ((neg >> n) & 1)) x = "(" + x + " ^ 0x80000000u)";
            return x;
        };
        const unsigned sources = op < 256 ? 2 : op >= 384 ? 1 : (op >= 320 && op < 384 && vop3) ? 3 : 2;
        std::string load = "uint a = " + operand(0) + "; uint b = " + (sources >= 2 ? operand(1) : std::string("0u")) +
                           "; uint c = " + (sources >= 3 ? operand(2) : std::string("0u")) +
                           "; float fa = gf(a); float fb = gf(b); float fc = gf(c); (void)a; (void)b; (void)c; (void)fa; (void)fb; (void)fc; ";
        const std::string E = ex();
        sregs_.insert(126);
        if (op < 256) { // compares
            const unsigned family = op >> 4, cond = op & 15;
            static const char* const iops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
            std::string r;
            if (family == 0 || family == 1 || family == 4 || family == 5) r = "gcn_cmpf(" + std::to_string(cond) + "u, fa, fb)";
            else if ((family == 8 || family == 9) && cond == 8) r = "gcn_class(fa, b)";
            else if ((family == 8 || family == 9 || family == 12 || family == 13) && cond < 8) {
                const bool sign = family < 12;
                if (cond == 0) r = "false"; else if (cond == 7) r = "true";
                else r = sign ? "int(a) " + std::string(iops[cond]) + " int(b)" : "a " + std::string(iops[cond]) + " b";
            } else return false;
            body_ += "{ bool cr = false; if (" + E + ") { " + load + "cr = " + r + "; } ";
            setScalar64(sdst, "cr ? 1ul : 0ul");
            if (family & 1) { execMayKill_=true; body_ += "s126 = cr ? 1u : 0u; " + s(127) + " = 0u; "; }
            body_ += "}";
            return out_.error.empty();
        }
        if (op == 384 + 2) { setScalar(dst, source(src[0], i)); return out_.error.empty(); } // per-lane approximation
        if (op == 384 + 0 || op == 384 + 65) { body_ += "/* nop */"; return true; }
        std::string expr, carry;
        const std::string cin = (op >= 296 && op <= 298) ? (vop3 ? "(" + scalar64(src[2], i) + " & 1ul)" : "ulong(" + s(106) + " & 1u)") : "0ul";
        const std::string D = v(dst);
        switch (op) {
        case 256: expr = "(" + (vop3 ? "(" + scalar64(src[2], i) + " & 1ul) != 0ul" : "(" + s(106) + " & 1u) != 0u") + ") ? b : a"; break;
        case 259: expr = "gu(fa + fb)"; break;
        case 260: expr = "gu(fa - fb)"; break;
        case 261: expr = "gu(fb - fa)"; break;
        case 263: expr = "(fa == 0.0f || fb == 0.0f) ? 0u : gu(fa * fb)"; break;
        case 264: expr = "gu(fa * fb)"; break;
        case 267: expr = "(a & 0xffffffu) * (b & 0xffffffu)"; break;
        case 268: expr = "uint((ulong(a & 0xffffffu) * ulong(b & 0xffffffu)) >> 32)"; break;
        case 271: expr = "gu(gcn_fmin(fa, fb))"; break;
        case 272: expr = "gu(gcn_fmax(fa, fb))"; break;
        case 273: expr = "uint(int(a) < int(b) ? int(a) : int(b))"; break;
        case 274: expr = "uint(int(a) > int(b) ? int(a) : int(b))"; break;
        case 275: expr = "a < b ? a : b"; break;
        case 276: expr = "a > b ? a : b"; break;
        case 277: expr = "a >> (b & 31u)"; break;
        case 278: expr = "b >> (a & 31u)"; break;
        case 279: expr = "uint(int(a) >> (b & 31u))"; break;
        case 280: expr = "uint(int(b) >> (a & 31u))"; break;
        case 281: expr = "a << (b & 31u)"; break;
        case 282: expr = "b << (a & 31u)"; break;
        case 283: expr = "a & b"; break;
        case 284: expr = "a | b"; break;
        case 285: expr = "a ^ b"; break;
        case 286: expr = "((1u << (a & 31u)) - 1u) << (b & 31u)"; break;
        case 287: expr = "gu(fa * fb + gf(" + D + "))"; break;
        case 288: expr = "gu(fa * gf(" + hex(i.literal) + ") + fb)"; break;
        case 289: expr = "gu(fa * fb + gf(" + hex(i.literal) + "))"; break;
        case 290: expr = "gcn_popcount(a) + b"; break;
        case 293: carry = "ulong t = ulong(a) + ulong(b); r = uint(t); co = uint(t >> 32);"; break;
        case 294: carry = "r = a - b; co = b > a ? 1u : 0u;"; break;
        case 295: carry = "r = b - a; co = a > b ? 1u : 0u;"; break;
        case 296: carry = "ulong t = ulong(a) + ulong(b) + " + cin + "; r = uint(t); co = uint(t >> 32);"; break;
        case 297: carry = "ulong sub = ulong(b) + " + cin + "; r = uint(ulong(a) - sub); co = sub > ulong(a) ? 1u : 0u;"; break;
        case 298: carry = "ulong sub = ulong(a) + " + cin + "; r = uint(ulong(b) - sub); co = sub > ulong(b) ? 1u : 0u;"; break;
        case 299: expr = "gu(gcn_ldexp(fa, int(b)))"; break;
        case 303: expr = "gcn_f16rtz(fa) | (gcn_f16rtz(fb) << 16)"; break;
        case 320: expr = "(fa == 0.0f || fb == 0.0f) ? gu(fc) : gu(fa * fb + fc)"; break;
        case 321: expr = "gu(fa * fb + fc)"; break;
        case 323: expr = "(a & 0xffffffu) * (b & 0xffffffu) + c"; break;
        case 324: case 325: case 326: case 327: expr = "gcn_cube(" + std::to_string(op) + "u, fa, fb, fc)"; break;
        case 328: expr = "(c & 31u) != 0u ? (a >> (b & 31u)) & ((1u << (c & 31u)) - 1u) : 0u"; break;
        case 330: expr = "(a & b) | (~a & c)"; break;
        case 331: expr = "gu(gcn_fma(fa, fb, fc))"; break;
        case 334: expr = "uint(((ulong(a) << 32) | ulong(b)) >> (c & 31u))"; break;
        case 337: expr = "gu(gcn_fmin(gcn_fmin(fa, fb), fc))"; break;
        case 340: expr = "gu(gcn_fmax(gcn_fmax(fa, fb), fc))"; break;
        case 343: expr = "gu(gcn_med3(fa, fb, fc))"; break;
        case 349: expr = "(a > b ? a - b : b - a) + c"; break;
        case 361: case 363: expr = "a * b"; break;
        case 362: expr = "uint((ulong(a) * ulong(b)) >> 32)"; break;
        case 374: case 375: {
            if (!vop3) return false;
            const std::string c64 = src[2] >= 256 ? "gcn_u64(" + v(src[2] - 256) + ", " + v(src[2] - 255) + ")" : scalar64(src[2], i);
            const std::string prod = op == 374 ? "ulong(a) * ulong(b)" : "gcn_mul_i64(a, b)";
            body_ += "{ if (" + E + ") { " + load + "ulong t = " + prod + " + " + c64 + "; " + D + " = uint(t); " +
                     v(dst + 1) + " = uint(t >> 32); } ";
            setScalar64(sdst, "0ul");
            body_ += "}";
            return out_.error.empty();
        }
        case 385: expr = "a"; break;
        case 389: expr = "gu(float(int(a)))"; break;
        case 390: expr = "gu(float(a))"; break;
        case 391: expr = "gcn_cvt_u32(fa)"; break;
        case 392: expr = "uint(gcn_cvt_i32(fa))"; break;
        case 394: expr = "gcn_f16rtz(fa)"; break;
        case 395: expr = "gu(gcn_f16(a & 0xffffu))"; break;
        case 396: expr = "uint(gcn_cvt_i32(gcn_floor(fa + 0.5f)))"; break;
        case 397: expr = "uint(gcn_cvt_i32(gcn_floor(fa)))"; break;
        case 398: expr = "gu(float(int(a << 28) >> 28) / 16.0f)"; break;
        case 401: case 402: case 403: case 404: expr = "gu(float((a >> " + std::to_string(8 * (op - 401)) + "u) & 0xffu))"; break;
        case 416: expr = "gu(fa - gcn_floor(fa))"; break;
        case 417: expr = "gu(gcn_trunc(fa))"; break;
        case 418: expr = "gu(gcn_ceil(fa))"; break;
        case 419: expr = "gu(gcn_rint(fa))"; break;
        case 420: expr = "gu(gcn_floor(fa))"; break;
        case 421: expr = "gu(gcn_exp2(fa))"; break;
        case 422: case 423: expr = "gu(gcn_log2(fa))"; break;
        case 424: case 425: case 426: case 427: expr = "gu(1.0f / fa)"; break;
        case 428: case 429: case 430: expr = "gu(1.0f / gcn_sqrt(fa))"; break;
        case 435: expr = "gu(gcn_sqrt(fa))"; break;
        case 437: expr = "gu(gcn_sin(fa * 6.28318530718f))"; break;
        case 438: expr = "gu(gcn_cos(fa * 6.28318530718f))"; break;
        case 439: expr = "~a"; break;
        case 440: expr = "gcn_bfrev(a)"; break;
        case 441: expr = "gcn_ffbh(a)"; break;
        case 442: expr = "gcn_ffbl(a)"; break;
        default: return false;
        }
        if (!carry.empty()) {
            body_ += "{ uint co = 0u; if (" + E + ") { " + load + "uint r = 0u; " + carry + " " + D + " = r; } ";
            setScalar64(sdst, "ulong(co)");
            body_ += "}";
            return out_.error.empty();
        }
        std::string result = expr;
        if (floatOut && (clamp || omod)) result = "gcn_finish(" + expr + ", " + std::to_string(omod) + "u, " + (clamp ? "true" : "false") + ")";
        body_ += "if (" + E + ") { " + load + D + " = " + result + "; }";
        return out_.error.empty();
    }

    bool vintrp(const Inst& in) {
        if (options_.stage != GCNStage::pixel) return fail("interpolation outside a pixel program");
        const GCNInstruction& i = in.i;
        const unsigned dst = (i.word0 >> 18) & 0xff, src = i.word0 & 0xff;
        const unsigned attr = (i.word0 >> 10) & 0x3f, chan = (i.word0 >> 8) & 3;
        out_.attributes |= 1u << (attr & 31);
        const std::string at = std::to_string(attr) + "u, " + std::to_string(chan) + "u, ";
        std::string r;
        if (i.opcode == 0) r = "gu(gcn_interp1(L, " + at + "gf(" + v(src) + ")))";
        else if (i.opcode == 1) r = "gu(gcn_interp2(L, " + at + "gf(" + v(src) + "), gf(" + v(dst) + ")))";
        else { r = "gu(gcn_interp_mov(L, " + at + std::to_string(src & 3) + "u))"; out_.flat |= 1u << (attr & 31); }
        body_ += "if (" + std::string(ex()) + ") " + v(dst) + " = " + r + ";";
        sregs_.insert(126);
        return true;
    }
    bool ds(const Inst& in) {
        const GCNInstruction& i = in.i;
        const unsigned offset = i.word0 & 0xffff;
        if (i.opcode != 53 || !(offset & 0x8000)) return fail("LDS access other than a quad swizzle");
        if (options_.stage != GCNStage::pixel) return fail("quad swizzle outside a pixel program");
        const unsigned dst = i.word1 >> 24, src = i.word1 & 0xff;
        // Every lane takes part (inactive ones contribute 0), so quads stay converged.
        body_ += "{ uint lane = (" + hex(offset & 0xff) + " >> (2u * L.quadLane)) & 3u; uint got = gcn_quad_shuffle(Q, L, " +
                 std::string(ex()) + " ? " + v(src) + " : 0u, lane); if (" + ex() + ") " + v(dst) + " = got; }";
        sregs_.insert(126);
        out_.quad = true;
        return true;
    }
    bool mubuf(const Inst& in) {
        const GCNInstruction& i = in.i;
        const std::uint32_t w = i.word0, w1 = i.word1;
        const unsigned vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, rsrc = ((w1 >> 16) & 0x1f) * 4;
        const bool offen = (w >> 12) & 1, idxen = (w >> 13) & 1;
        if ((w >> 15) & 1) return fail("addr64 buffer access");
        const unsigned op = i.opcode;
        unsigned count;
        const bool store = (op >= 4 && op <= 7) || op == 24 || op == 26 || (op >= 28 && op <= 31);
        if (op <= 3) count = op + 1;
        else if (op >= 4 && op <= 7) count = op - 3;
        else if (op == 12 || op == 24 || op == 26 || op == 28) count = 1;
        else if (op == 13 || op == 29) count = 2; else if (op == 14 || op == 30) count = 4; else if (op == 15 || op == 31) count = 3;
        else return fail("unsupported buffer access");
        if (store && options_.stage != GCNStage::compute) return fail("buffer store outside a compute program");
        const std::string res = "GCNRes4 r = {{" + s(rsrc) + ", " + s(rsrc + 1) + ", " + s(rsrc + 2) + ", " + s(rsrc + 3) + "}}; ";
        const std::string index = idxen ? v(vaddr) : "0u", voffset = offen ? v(vaddr + (idxen ? 1 : 0)) : "0u";
        const std::string args = index + ", " + voffset + ", " + scalar(w1 >> 24, i) + ", " + std::to_string(w & 0xfff) + "u, " +
                                 (idxen ? "true" : "false") + ", " + (offen ? "true" : "false");
        body_ += "if (" + std::string(ex()) + ") { " + res;
        if (store) {
            out_.stores = true;
            if (op <= 7) {
                body_ += "gcn_buffer_store_format(M, r, " + args + ", uint4(";
                for (unsigned c = 0; c < 4; ++c) body_ += (c < count ? v(vdata + c) : std::string("0u")) + (c < 3 ? ", " : "), ");
                body_ += std::to_string(count) + "u); }";
            } else if (op == 24 || op == 26) {
                body_ += "gcn_buffer_store_bytes(M, r, " + args + ", " + v(vdata) + ", " + (op == 24 ? "1u" : "2u") + "); }";
            } else {
                for (unsigned c = 0; c < count; ++c)
                    body_ += "gcn_buffer_store_dword(M, r, " + args + ", " + std::to_string(c) + "u, " + v(vdata + c) + "); ";
                body_ += "}";
            }
            sregs_.insert(126);
            return out_.error.empty();
        }
        if (op <= 3) {
            body_ += "uint4 f = gcn_buffer_format(M, r, " + args + "); ";
            for (unsigned c = 0; c < count; ++c) body_ += v(vdata + c) + " = f[" + std::to_string(c) + "]; ";
        } else {
            for (unsigned c = 0; c < count; ++c)
                body_ += v(vdata + c) + " = gcn_buffer_dword(M, r, " + args + ", " + std::to_string(c) + "u); ";
        }
        body_ += "}";
        sregs_.insert(126);
        return out_.error.empty();
    }
    bool mimg(const Inst& in) {
        const GCNInstruction& i = in.i;
        const std::uint32_t w = i.word0, w1 = i.word1;
        const unsigned vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff;
        const unsigned rsrc = ((w1 >> 16) & 0x1f) * 4, samp = ((w1 >> 21) & 0x1f) * 4, dmask = (w >> 8) & 15;
        const unsigned op = i.opcode;
        unsigned first = 0;
        if (op == 37 || op == 38 || op == 53 || op == 54) first = 1;
        if (op >= 48 && op < 64) first += 1;
        if ((op >= 40 && op < 48) || (op >= 56 && op < 64)) return fail("depth-compare sampling");
        std::string res = "GCNRes8 t = {{";
        for (unsigned n = 0; n < 8; ++n) res += s(rsrc + n) + (n < 7 ? ", " : "}}; ");
        std::string call;
        if (op >= 32 && op < 64) {
            res += "GCNRes4 sm = {{" + s(samp) + ", " + s(samp + 1) + ", " + s(samp + 2) + ", " + s(samp + 3) + "}}; ";
            call = "gcn_sample(M, t, sm, " + std::string(((w >> 12) & 1) ? "true" : "false") + ", gf(" + v(vaddr + first) + "), gf(" +
                   v(vaddr + first + 1) + "))";
        } else if (op == 0 || op == 1) {
            call = "gcn_image_load(M, t, " + v(vaddr) + ", " + v(vaddr + 1) + ")";
        } else if (op == 8 || op == 9) {
            // image_store(_mip): the enabled components, in order, from vdata.
            if (options_.stage != GCNStage::compute) return fail("image store outside a compute program");
            out_.stores = true;
            body_ += "if (" + std::string(ex()) + ") { " + res + "gcn_image_store(M, t, " + v(vaddr) + ", " + v(vaddr + 1) + ", uint4(";
            for (unsigned n = 0, in = 0; n < 4; ++n)
                body_ += ((dmask >> n) & 1 ? v(vdata + in++) : std::string("0u")) + (n < 3 ? ", " : "), ");
            body_ += std::to_string(dmask) + "u); }";
            sregs_.insert(126);
            return out_.error.empty();
        } else if (op == 14) {
            // image_get_resinfo: integer sizes, not texels.
            body_ += "if (" + std::string(ex()) + ") { " + res + "uint4 c = gcn_image_resinfo(t, " + v(vaddr) + "); ";
            for (unsigned n = 0, out = 0; n < 4; ++n)
                if ((dmask >> n) & 1) body_ += v(vdata + out++) + " = c[" + std::to_string(n) + "]; ";
            body_ += "}";
            sregs_.insert(126);
            return out_.error.empty();
        } else return fail("unsupported image access " + in.name);
        body_ += "if (" + std::string(ex()) + ") { " + res + "float4 c = " + call + "; ";
        for (unsigned n = 0, out = 0; n < 4; ++n)
            if ((dmask >> n) & 1) body_ += v(vdata + out++) + " = gu(c[" + std::to_string(n) + "]); ";
        body_ += "}";
        sregs_.insert(126);
        return out_.error.empty();
    }
    bool exportOp(const Inst& in) {
        const GCNInstruction& i = in.i;
        const unsigned target = (i.word0 >> 4) & 0x3f, enable = i.word0 & 15;
        const bool compressed = (i.word0 >> 10) & 1, done = (i.word0 >> 11) & 1;
        std::string r[4];
        for (unsigned c = 0; c < 4; ++c) r[c] = v((i.word1 >> (8 * c)) & 0xff);
        std::string value;
        if (compressed) value = "float4(gcn_f16(" + r[0] + " & 0xffffu), gcn_f16(" + r[0] + " >> 16), gcn_f16(" + r[1] +
                                " & 0xffffu), gcn_f16(" + r[1] + " >> 16))";
        else {
            value = "float4(";
            for (unsigned c = 0; c < 4; ++c)
                value += ((enable >> c) & 1 ? "gf(" + r[c] + ")" : std::string(c == 3 ? "1.0f" : "0.0f")) + (c < 3 ? ", " : ")");
        }
        const std::string E = ex();
        sregs_.insert(126);
        if (target < 8) {
            out_.colors |= 1u << target;
            body_ += "if (" + E + ") { L.color[" + std::to_string(target) + "] = " + value + "; L.colorWritten |= " +
                     std::to_string(1u << target) + "u; }";
            if (done) body_ += " else L.killed = true;";
        } else if (target == 9) {
            if (done) body_ += "if (!" + E + ") L.killed = true;";
        } else if (target == 12) { out_.position = true; body_ += "L.pos = " + value + ";"; }
        else if (target >= 32 && target < 64) {
            out_.params |= 1u << (target - 32);
            body_ += "L.param[" + std::to_string(target - 32) + "] = " + value + ";";
        } else if (target >= 13 && target <= 15) {
            body_ += "/* auxiliary position export ignored */";
        } else return fail("export target " + std::to_string(target));
        return true;
    }
};

inline GCNTranslation translateGCN(const GCNMemory& memory, std::uint64_t entry, const GCNTranslateOptions& options) {
    return GCNTranslator(memory, options).translate(entry);
}

// The Metal kernel gcn_compute around a translated compute program (after
// gcnMetalPrelude()). Buffers: 0 guest memory, writable: one window at
// `base` or rangeCount copied ranges, as for draws; 1 GCNDispatchState. COMPUTE_PGM_RSRC2 (rsrc2) enables the thread group ids,
// which follow the user SGPRs, and the thread id components beyond x.
inline std::string gcnMetalComputeEntry(const std::string& name) {
    return "struct GCNDispatchState { ulong base; ulong size; uint user[16]; uint userCount; uint rsrc2; uint rangeCount; uint pad; ulong ranges[48]; };\n"
           "kernel void gcn_compute(uint3 group [[threadgroup_position_in_grid]], uint3 local [[thread_position_in_threadgroup]],\n"
           "    device uchar* mem [[buffer(0)]], constant GCNDispatchState& D [[buffer(1)]]) {\n"
           "    GCNLane L; gcn_lane_init(L);\n"
           "    uint n = min(D.userCount, 16u);\n"
           "    for (uint k = 0u; k < n; ++k) L.user[k] = D.user[k];\n"
           "    if ((D.rsrc2 & 0x80u) != 0u && n < 20u) L.user[n++] = group.x;\n"
           "    if ((D.rsrc2 & 0x100u) != 0u && n < 20u) L.user[n++] = group.y;\n"
           "    if ((D.rsrc2 & 0x200u) != 0u && n < 20u) L.user[n++] = group.z;\n"
           "    L.vinit[0] = local.x; L.vinit[1] = local.y; L.vinit[2] = local.z;\n"
           "    GCNMemory M = { mem, D.base, D.size, D.ranges, D.rangeCount }; GCNQuad Q = { 0u };\n"
           "    " + name + "(L, M, Q);\n}\n";
}

// One past the highest bit set in `bits` (at least 1): an array size.
inline unsigned gcnHighestUsed(std::uint32_t bits) { unsigned n = 1; while (n < 32 && (bits >> n)) ++n; return n; }
// The Metal build of GCNShaderRuntime.hpp: put it before translated programs.
// `params`, `attrs` and `colors` size the lane's parameter, attribute and
// colour arrays: one past the highest the library's programs use. A lane
// sized for 32 of each (1.3 KiB per invocation) made Metal's vertex stage
// rasterize nothing for some programs in copy mode.
inline std::string gcnMetalPrelude(bool nativeTextures = false, unsigned params = 32, unsigned attrs = 32, unsigned colors = 8) {
    std::string text = "#define GCN_PARAMS " + std::to_string(params ? params : 1) + "u\n#define GCN_ATTRS " +
                       std::to_string(attrs ? attrs : 1) + "u\n#define GCN_COLORS " + std::to_string(colors ? colors : 1) + "u\n";
    text += R"MSL(#include <metal_stdlib>
using namespace metal;
#define GCN_NATIVE_TEXTURES 0
#define GCN_THREAD thread
#define GCN_FN inline
struct GCNRes8 { uint d[8]; };
struct GCNRes4 { uint d[4]; };
#if GCN_NATIVE_TEXTURES
// Up to three of a draw's T#s viewed as Metal textures (a video's Y, U and V
// planes); bit k of `enabled` marks texture k valid, bit 4 + k its Metal
// sampler, built on the CPU from S# `sdesc` (the one the draw samples it with).
struct GCNNativeTexture {
    texture2d<float> image [[id(0)]];
    uint enabled [[id(1)]];
    uint4 low [[id(2)]];
    uint4 high [[id(3)]];
    texture2d<float> image1 [[id(4)]];
    uint4 low1 [[id(5)]];
    uint4 high1 [[id(6)]];
    texture2d<float> image2 [[id(7)]];
    uint4 low2 [[id(8)]];
    uint4 high2 [[id(9)]];
    sampler samp [[id(10)]];
    sampler samp1 [[id(11)]];
    sampler samp2 [[id(12)]];
    uint4 sdesc [[id(13)]];
    uint4 sdesc1 [[id(14)]];
    uint4 sdesc2 [[id(15)]];
};
#endif
// Large ranges live in buffers of their own (buffer(5) up), shared by the
// draws of a submission: a range offset's bits 48 and up select one (0: p).
#define GCN_SLOTS 6u
struct GCNSlots { device const uchar* x[GCN_SLOTS]; };
#define GCN_SLOT_PARAMS , device const uchar* x0 [[buffer(5)]], device const uchar* x1 [[buffer(6)]], device const uchar* x2 [[buffer(7)]], \
    device const uchar* x3 [[buffer(8)]], device const uchar* x4 [[buffer(9)]], device const uchar* x5 [[buffer(10)]]
#define GCN_SLOT_ARGS { { x0, x1, x2, x3, x4, x5 } }
struct GCNMemory { device const uchar* p; ulong base; ulong size; constant ulong* ranges; uint count;
#if GCN_NATIVE_TEXTURES
    constant GCNNativeTexture* native;
#endif
    GCNSlots s;
};
GCN_FN device const uchar* gcn_at(GCNMemory M, ulong o) {
    uint k = uint(o >> 48); o &= 0xfffffffffffful;
    return k == 0u ? M.p + o : M.s.x[min(k, GCN_SLOTS) - 1u] + o;
}
struct GCNQuad { uint unused; };
struct GCNLane {
    uint user[20]; uint vinit[24]; bool exec; uint quadLane;
    float4 attr[GCN_ATTRS];
    float4 pos; float4 param[GCN_PARAMS]; float4 color[GCN_COLORS]; uint colorWritten; bool killed;
};
GCN_FN void gcn_clear(thread GCNLane& L) {
    for (uint k = 0u; k < 20u; ++k) L.user[k] = 0u;
    for (uint k = 0u; k < 24u; ++k) L.vinit[k] = 0u;
    for (uint k = 0u; k < GCN_ATTRS; ++k) L.attr[k] = float4(0.0f);
    for (uint k = 0u; k < GCN_PARAMS; ++k) L.param[k] = float4(0.0f);
    for (uint k = 0u; k < GCN_COLORS; ++k) L.color[k] = float4(0.0f);
    L.pos = float4(0.0f); L.exec = true; L.quadLane = 0u; L.colorWritten = 0u; L.killed = false;
}
// gcn_clear without attr/param/color: zeroing those arrays for every pixel
// was most of a full-screen draw's GPU time (GCNShaderCostMetalTest). Entry
// points set the attributes a program reads and clear what it exports.
GCN_FN void gcn_lane_init(thread GCNLane& L) {
    for (uint k = 0u; k < 20u; ++k) L.user[k] = 0u;
    for (uint k = 0u; k < 24u; ++k) L.vinit[k] = 0u;
    L.pos = float4(0.0f); L.exec = true; L.quadLane = 0u; L.colorWritten = 0u; L.killed = false;
}
// Per-draw state at buffer(1): guest memory at buffer(0) is either one window
// starting at guest address `base` (rangeCount 0) or rangeCount copied ranges
// as (guest start, length, offset in the buffer) triples; user SGPRs of each
// stage; GCN_DRAW_* flags.
struct GCNDrawState { ulong base; ulong size; uint vsUser[16]; uint psUser[17]; uint flags; uint rangeCount; ulong ranges[48]; };
#define GCN_DRAW_RECT_LIST 1u
#define GCN_DRAW_FLIP_Y 2u
GCN_FN float gf(uint x) { return as_type<float>(x); }
GCN_FN uint gu(float f) { return as_type<uint>(f); }
GCN_FN ulong gcn_u64(uint lo, uint hi) { return ulong(lo) | (ulong(hi) << 32); }
GCN_FN ulong gcn_mul_i64(uint a, uint b) { return ulong(long(int(a)) * long(int(b))); }
GCN_FN float gcn_floor(float x) { return floor(x); }
GCN_FN float gcn_ceil(float x) { return ceil(x); }
GCN_FN float gcn_trunc(float x) { return trunc(x); }
GCN_FN float gcn_rint(float x) { return rint(x); }
GCN_FN float gcn_exp2(float x) { return precise::exp2(x); }
GCN_FN float gcn_log2(float x) { return precise::log2(x); }
GCN_FN float gcn_sqrt(float x) { return precise::sqrt(x); }
GCN_FN float gcn_sin(float x) { return precise::sin(x); }
GCN_FN float gcn_cos(float x) { return precise::cos(x); }
GCN_FN float gcn_fmin(float a, float b) { return fmin(a, b); }
GCN_FN float gcn_fmax(float a, float b) { return fmax(a, b); }
GCN_FN float gcn_fma(float a, float b, float c) { return fma(a, b, c); }
GCN_FN float gcn_ldexp(float a, int e) { return ldexp(a, e); }
GCN_FN bool gcn_isnan(float a) { return isnan(a); }
GCN_FN float gcn_med3(float a, float b, float c) { return gcn_fmax(gcn_fmin(a, b), gcn_fmin(gcn_fmax(a, b), c)); }
GCN_FN uint gcn_popcount(uint v) { return popcount(v); }
GCN_FN uint gcn_bfrev(uint a) { return reverse_bits(a); }
GCN_FN uint gcn_ffbh(uint a) { return a == 0u ? 0xffffffffu : clz(a); }
GCN_FN uint gcn_ffbl(uint a) { return a == 0u ? 0xffffffffu : ctz(a); }
GCN_FN bool gcn_locate(GCNMemory M, ulong a, ulong n, thread ulong& offset) {
    if (M.count == 0u) {
        if (a < M.base || a - M.base > M.size || M.size - (a - M.base) < n) return false;
        offset = a - M.base; return true;
    }
    for (uint k = 0u; k < M.count; ++k) {
        ulong start = M.ranges[3u * k]; ulong length = M.ranges[3u * k + 1u];
        if (a >= start && a - start <= length && length - (a - start) >= n) { offset = M.ranges[3u * k + 2u] + (a - start); return true; }
    }
    return false;
}
GCN_FN uint gcn_ld32(GCNMemory M, ulong a) { ulong o = 0ul; if (!gcn_locate(M, a, 4ul, o)) return 0u; return *(device const uint*)gcn_at(M, o); }
GCN_FN uint gcn_ld16(GCNMemory M, ulong a) { ulong o = 0ul; if (!gcn_locate(M, a, 2ul, o)) return 0u; return uint(*(device const ushort*)gcn_at(M, o)); }
GCN_FN uint gcn_ld8(GCNMemory M, ulong a) { ulong o = 0ul; if (!gcn_locate(M, a, 1ul, o)) return 0u; return uint(*gcn_at(M, o)); }
// Only used with an image span already validated by gcn_locate.
GCN_FN uint gcn_packed32(GCNMemory M, ulong o) { return *(device const uint*)gcn_at(M, o); }
GCN_FN uint gcn_packed16(GCNMemory M, ulong o) { return uint(*(device const ushort*)gcn_at(M, o)); }
GCN_FN uint gcn_packed8(GCNMemory M, ulong o) { return uint(*gcn_at(M, o)); }
// Stores, from compute kernels only, whose memory buffer (never a slot) is writable.
GCN_FN void gcn_st32(GCNMemory M, ulong a, uint v) { ulong o = 0ul; if (gcn_locate(M, a, 4ul, o)) *(device uint*)(M.p + o) = v; }
GCN_FN void gcn_st16(GCNMemory M, ulong a, uint v) { ulong o = 0ul; if (gcn_locate(M, a, 2ul, o)) *(device ushort*)(M.p + o) = ushort(v); }
GCN_FN void gcn_st8(GCNMemory M, ulong a, uint v) { ulong o = 0ul; if (gcn_locate(M, a, 1ul, o)) *(device uchar*)(M.p + o) = uchar(v); }
#if GCN_NATIVE_TEXTURES
// Which of the draw's Metal textures (0 to 2) views T# `t`, or -1.
GCN_FN int gcn_native_match(GCNMemory M, GCNRes8 t) {
    const uint dfmt = (t.d[1] >> 20) & 63u;
    if (!M.native || M.native->enabled == 0u || (dfmt != 10u && dfmt != 16u && dfmt != 1u)) return -1;
    const uint4 low = uint4(t.d[0],t.d[1],t.d[2],t.d[3]), high = uint4(t.d[4],t.d[5],t.d[6],t.d[7]);
    const uint enabled = M.native->enabled;
    if ((enabled & 1u) && all(M.native->low == low) && all(M.native->high == high)) return 0;
    if ((enabled & 2u) && all(M.native->low1 == low) && all(M.native->high1 == high)) return 1;
    if ((enabled & 4u) && all(M.native->low2 == low) && all(M.native->high2 == high)) return 2;
    return -1;
}
// 5_6_5 viewed as B5G6R5Unorm: GCN's first component is the low 5 bits, Metal's blue.
GCN_FN float4 gcn_native_order(GCNRes8 t, float4 value) {
    return ((t.d[1] >> 20) & 63u) == 16u ? float4(value.b, value.g, value.r, 1.0f) : value;
}
GCN_FN bool gcn_native_read(texture2d<float> image, GCNRes8 t, int x, int y, thread float4& value) {
    if (x < 0 || y < 0 || uint(x) >= image.get_width() || uint(y) >= image.get_height()) return false;
    value = gcn_native_order(t, image.read(uint2(x,y)));
    return true;
}
// The whole sample filtered by Metal's sampler (one sample instruction: the
// CPU built it from this S#): the texture must be exactly the T#'s size
// (8-bit filtering, within a level of GCN's own). False: gcn_sample filters itself.
GCN_FN bool gcn_native_filter(texture2d<float> image, sampler q, bool usable, GCNRes8 t, GCNRes4 s, bool unorm, float u, float v,
                              thread float4& value) {
    const uint w = (t.d[2] & 0x3fffu) + 1u, h = ((t.d[2] >> 14) & 0x3fffu) + 1u;
    if (!usable || image.get_width() != w || image.get_height() != h) return false;
    const bool un = unorm || ((s.d[0] >> 15) & 1u) != 0u;
    const float2 uv = un ? float2(u / float(w), v / float(h)) : float2(u, v);
    value = gcn_native_order(t, image.sample(q, uv, level(0.0f)));
    return true;
}
#endif
GCN_FN bool gcn_native_texel(GCNMemory M, GCNRes8 t, int x, int y, thread float4& value) {
#if GCN_NATIVE_TEXTURES
    const int k = gcn_native_match(M, t);
    if (k < 0) return false;
    return k == 0 ? gcn_native_read(M.native->image, t, x, y, value) :
           k == 1 ? gcn_native_read(M.native->image1, t, x, y, value) : gcn_native_read(M.native->image2, t, x, y, value);
#else
    return false;
#endif
}
GCN_FN bool gcn_native_sample(GCNMemory M, GCNRes8 t, GCNRes4 s, bool unorm, float u, float v, thread float4& value) {
#if GCN_NATIVE_TEXTURES
    const int k = gcn_native_match(M, t);
    if (k < 0) return false;
    const uint4 sd = uint4(s.d[0], s.d[1], s.d[2], s.d[3]);
    const bool usable = ((M.native->enabled >> (4 + k)) & 1u) != 0u &&
        all(sd == (k == 0 ? M.native->sdesc : k == 1 ? M.native->sdesc1 : M.native->sdesc2));
    return k == 0 ? gcn_native_filter(M.native->image, M.native->samp, usable, t, s, unorm, u, v, value) :
           k == 1 ? gcn_native_filter(M.native->image1, M.native->samp1, usable, t, s, unorm, u, v, value) :
                    gcn_native_filter(M.native->image2, M.native->samp2, usable, t, s, unorm, u, v, value);
#else
    return false;
#endif
}
// Attributes arrive interpolated by Metal: p1 is a no-op, p2 the value.
GCN_FN float gcn_interp1(thread GCNLane& L, uint attr, uint chan, float i) { return 0.0f; }
GCN_FN float gcn_interp2(thread GCNLane& L, uint attr, uint chan, float j, float previous) { return L.attr[min(attr, GCN_ATTRS - 1u)][chan & 3u]; }
GCN_FN float gcn_interp_mov(thread GCNLane& L, uint attr, uint chan, uint select) { return L.attr[min(attr, GCN_ATTRS - 1u)][chan & 3u]; }
GCN_FN bool gcn_quad_any(thread GCNQuad& Q, thread GCNLane& L, bool b) { return quad_any(b); }
GCN_FN uint gcn_quad_shuffle(thread GCNQuad& Q, thread GCNLane& L, uint value, uint lane) { return quad_shuffle(value, ushort(lane)); }
)MSL";
    static const char* const shared =
#define GCN_SHARED_SOURCE(...) #__VA_ARGS__
#include "GCNShaderRuntimeShared.inc"
#undef GCN_SHARED_SOURCE
        ;
    if (nativeTextures) {
        const std::string flag = "#define GCN_NATIVE_TEXTURES 0";
        text.replace(text.find(flag), flag.size(), "#define GCN_NATIVE_TEXTURES 1");
    }
    return text + shared + "\n";
}

// Metal entry points gcn_vertex and gcn_fragment around a translated vertex
// and pixel program (their sources must follow gcnMetalPrelude()). Buffers:
// 0 the guest memory window, 1 GCNDrawState, 2 (vertex) the vertex indices.
// Vertex invocation n runs the VS on indices[n]; with GCN_DRAW_RECT_LIST, six
// invocations per three indices draw each rectangle, its fourth corner being
// v1 + v2 - v0 (the VS runs on the three given corners); GCN_DRAW_FLIP_Y
// negates clip-space y. psInputAddr is SPI_PS_INPUT_ADDR and inputCntl
// SPI_PS_INPUT_CNTL_0..31, read when the pipeline is built. The fragment
// returns colour target 0.
inline std::string gcnMetalEntryPoints(const GCNTranslation& vs, const GCNTranslation& ps, const std::string& vsName,
                                       const std::string& psName, std::uint32_t psInputAddr, const std::uint32_t inputCntl[32], bool nativeTextures = false) {
    std::uint32_t flatParams = 0;
    for (unsigned a = 0; a < 32; ++a)
        if (((ps.attributes >> a) & 1) && ((ps.flat >> a) & 1) && !(inputCntl[a] & 0x20)) flatParams |= 1u << (inputCntl[a] & 0x1f);
    std::string s = "struct GCNVaryings {\n    float4 position [[position]];\n";
    for (unsigned p = 0; p < 32; ++p)
        if ((vs.params >> p) & 1)
            s += "    float4 p" + std::to_string(p) + " [[user(locn" + std::to_string(p) + ")" + ((flatParams >> p) & 1 ? ", flat" : "") + "]];\n";
    s += "};\n"
         "GCN_FN GCNVaryings gcn_run_vertex(uint index, uint iid, device const uchar* mem, thread const GCNSlots& S, constant GCNDrawState& D) {\n"
         "    GCNLane L; gcn_lane_init(L);\n";
    for (unsigned p = 0; p < 32; ++p)
        if ((vs.params >> p) & 1) s += "    L.param[" + std::to_string(p) + "] = float4(0.0f);\n";
    s += "    for (uint k = 0u; k < 16u; ++k) L.user[k] = D.vsUser[k];\n"
         "    L.vinit[0] = index; L.vinit[3] = iid;\n"
         "    GCNMemory M = { mem, D.base, D.size, D.ranges, D.rangeCount }; GCNQuad Q = { 0u }; M.s = S;\n"
         "    " + vsName + "(L, M, Q);\n"
         "    GCNVaryings o; o.position = L.pos;\n";
    for (unsigned p = 0; p < 32; ++p)
        if ((vs.params >> p) & 1) s += "    o.p" + std::to_string(p) + " = L.param[" + std::to_string(p) + "];\n";
    s += "    return o;\n}\n"
         "vertex GCNVaryings gcn_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
         "    device const uchar* mem [[buffer(0)]], constant GCNDrawState& D [[buffer(1)]],\n"
         "    device const uint* indices [[buffer(2)]] GCN_SLOT_PARAMS) {\n"
         "    GCNSlots S = GCN_SLOT_ARGS;\n"
         // One call site for the program, in a loop: with a call per corner
         // (four inlined copies) Metal miscompiled large vertex programs and
         // rasterized nothing (Mortal Kombat: Legacy Kollection's matrix VS).
         "    uint at[3] = { vid, 0u, 0u }; uint n = 1u;\n"
         "    if ((D.flags & GCN_DRAW_RECT_LIST) != 0u) {\n"
         "        uint first = 3u * (vid / 6u); uint k = vid % 6u;\n"
         "        uint corner = k == 0u ? 0u : k == 1u ? 1u : k == 2u ? 2u : k == 3u ? 2u : k == 4u ? 1u : 3u;\n"
         "        if (corner < 3u) at[0] = first + corner;\n"
         "        else { n = 3u; at[0] = first; at[1] = first + 1u; at[2] = first + 2u; }\n"
         "    }\n"
         "    GCNVaryings got[3];\n"
         "    for (uint j = 0u; j < n; ++j) got[j] = gcn_run_vertex(indices[at[j]], iid, mem, S, D);\n"
         "    GCNVaryings o = got[0];\n"
         "    if (n == 3u) {\n"
         "        o.position = got[1].position + got[2].position - got[0].position;\n";
    for (unsigned p = 0; p < 32; ++p)
        if ((vs.params >> p) & 1) s += "        o.p" + std::to_string(p) + " = got[1].p" + std::to_string(p) + " + got[2].p" + std::to_string(p) + " - got[0].p" + std::to_string(p) + ";\n";
    s += "    }\n"
         "    if ((D.flags & GCN_DRAW_FLIP_Y) != 0u) o.position.y = -o.position.y;\n"
         "    return o;\n}\n"
         "struct GCNFragment { float4 color [[color(0)]]; uint coverage [[sample_mask]]; };\n";
    s += std::string("fragment ")+(ps.mayKill ? "GCNFragment" : "float4")+
         " gcn_fragment(GCNVaryings in [[stage_in]], uint sampleCoverage [[sample_mask]],\n"
         "    device const uchar* mem [[buffer(0)]], constant GCNDrawState& D [[buffer(1)]] GCN_SLOT_PARAMS) {\n"
         // Incoming raster coverage is independent of later fragment kills.
         "    const bool covered = sampleCoverage != 0u;\n"
         "    GCNLane L; gcn_lane_init(L);\n";
    for (unsigned c = 0; c < 8; ++c)
        if (c == 0 || ((ps.colors >> c) & 1)) s += "    L.color[" + std::to_string(c) + "] = float4(0.0f);\n";
    s += "    for (uint k = 0u; k < 17u; ++k) L.user[k] = D.psUser[k];\n"
         "    L.exec = covered;\n"
         "    L.quadLane = (uint(in.position.x) & 1u) | ((uint(in.position.y) & 1u) << 1);\n";
    for (unsigned a = 0; a < 32; ++a) {
        if (!((ps.attributes >> a) & 1)) continue;
        const std::uint32_t cntl = inputCntl[a];
        std::string value;
        if (cntl & 0x20) {
            const unsigned dv = (cntl >> 8) & 3;
            value = std::string("float4(") + (dv >> 1 ? "1.0f, 1.0f, 1.0f, " : "0.0f, 0.0f, 0.0f, ") + (dv & 1 ? "1.0f)" : "0.0f)");
        } else if ((vs.params >> (cntl & 0x1f)) & 1) value = "in.p" + std::to_string(cntl & 0x1f);
        else value = "float4(0.0f)";
        s += "    L.attr[" + std::to_string(a) + "] = " + value + ";\n";
    }
    // Input VGPRs in SPI_PS_INPUT_ADDR order; barycentrics are unused (Metal interpolates).
    static const unsigned sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    unsigned reg = 0;
    for (unsigned bit = 0; bit < 16 && reg < 24; ++bit) {
        if (!((psInputAddr >> bit) & 1)) continue;
        const std::string at = "    L.vinit[" + std::to_string(reg) + "] = ";
        if (bit == 8) s += at + "gu(in.position.x);\n";
        else if (bit == 9) s += at + "gu(in.position.y);\n";
        else if (bit == 10) s += at + "gu(in.position.z);\n";
        else if (bit == 11) s += at + "gu(in.position.w);\n";
        else if (bit == 14) s += at + "1u;\n";
        reg += sizes[bit];
    }
    s += "    GCNMemory M = { mem, D.base, D.size, D.ranges, D.rangeCount }; GCNQuad Q = { 0u }; M.s = GCNSlots GCN_SLOT_ARGS;\n"
         "    " + psName + "(L, M, Q);\n";
    // An ordinary shader's saved-EXEC/WQM/restore sequence cannot kill a
    // covered pixel. Do not ask Metal to suppress helpers again at export.
    s += ps.mayKill ? "    return { L.color[0], L.killed ? 0u : 0xffffffffu };\n}\n"
                    : "    return L.color[0];\n}\n";
    if (nativeTextures) {
        auto replace = [&](const std::string& from, const std::string& to) {
            for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
        };
        replace("constant GCNDrawState& D) {", "constant GCNDrawState& D, constant GCNNativeTexture& N) {");
        replace("device const uint* indices [[buffer(2)]] GCN_SLOT_PARAMS) {", "device const uint* indices [[buffer(2)]], constant GCNNativeTexture& N [[buffer(3)]] GCN_SLOT_PARAMS) {");
        replace("constant GCNDrawState& D [[buffer(1)]] GCN_SLOT_PARAMS) {", "constant GCNDrawState& D [[buffer(1)]], constant GCNNativeTexture& N [[buffer(3)]] GCN_SLOT_PARAMS) {");
        replace("iid, mem, S, D)", "iid, mem, S, D, N)");
        replace("D.rangeCount }", "D.rangeCount, &N }");
    }
    return s;
}
} // namespace maxps4::gpu
