#include "../Native/GCNDecoder.hpp"
#include <cassert>
#include <cstdio>
using namespace maxps4::gpu;
int main() {
    // shadPS4's embedded full-screen VS (sceGnmSetEmbeddedVsShader), with its
    // published disassembly, followed by its OrbShdr binary information.
    const std::uint32_t vs[] = {
        0xbeeb03ff, 0x00000007, 0x36020081, 0x34020281, 0x360000c2, 0x4a0202c1, 0x4a0000c1, 0x7e020b01,
        0x7e000b00, 0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020001, 0xf800020f, 0x03030303, 0xbf810000,
        0x5362724f, 0x07726468, 0x00004047, 0, 0x47f8c29f, 0x9b2da5cf, 0xff7c5b7d, 0x00000017};
    GCNProgram program;
    assert(locateGCNProgram(vs, 24, program) && program.codeWords == 16 && program.type == 1);
    assert(!locateGCNProgram(vs, 18, program) && !locateGCNProgram(vs + 1, 23, program));
    const auto decoded = decodeGCN(vs, program.codeWords);
    assert(decoded && decoded.instructions.size() == 13);
    const char* expected[] = {
        "s_mov_b32 vcc_hi, 0x7", "v_and_b32 v1, 1, v0", "v_lshlrev_b32 v1, 1, v1", "v_and_b32 v0, -2, v0",
        "v_add_i32 v1, vcc, -1, v1", "v_add_i32 v0, vcc, -1, v0", "v_cvt_f32_i32 v1, v1", "v_cvt_f32_i32 v0, v0",
        "v_mov_b32 v2, 0", "v_mov_b32 v3, 1.0", "exp pos0, v1, v0, v2, v3 done", "exp param0, v3, v3, v3, v3",
        "s_endpgm"};
    for (std::size_t n = 0; n < 13; ++n) {
        const auto text = gcnDisassemble(decoded.instructions[n]);
        if (text != expected[n]) { std::printf("%zu: %s\n", n, text.c_str()); assert(false); }
    }
    assert(decoded.instructions[0].size == 2 && decoded.instructions[0].hasLiteral && decoded.instructions[0].literal == 7);
    assert(decoded.instructions[10].encoding == GCNEncoding::exp && decoded.instructions[10].word == 11);
    // Every other encoding family, hand-assembled.
    const std::uint32_t mixed[] = {
        0xbe802100,             // s_swappc_b64 s[0:1], s[0:1]
        0xc0820100,             // s_load_dwordx4 s[4:7], s[0:1], 0x0
        0xc2000904,             // s_buffer_load_dword s0, s[8:11], 0x4
        0xbf8c007f,             // s_waitcnt lgkmcnt(0)
        0xc8080100,             // v_interp_p1_f32 v2, v0, attr0.y
        0xf0800f00, 0x00410002, // image_sample v[0:3], v2, s[4:11], s[8:11] dmask:0xf
        0xe00c2000, 0x80010400, // buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
        0xd2100002, 0x00020200, // v_mul_f32 v2, s0, v1 (VOP3, opcode 256 + 8)
        0x7c021101,             // v_cmp_lt_f32 vcc, v1, v8
        0x320402ff, 0x3f000000, // v_lshl_b32 v2, 0x3f000000, v1 (VOP2 literal source)
        0x89000201,             // s_xor_b32 s0, s1, s2
        0xbf820002,             // s_branch 18
        0xbf810000};            // s_endpgm
    const auto more = decodeGCN(mixed, sizeof(mixed) / 4);
    assert(more && more.instructions.size() == 13);
    const char* expectedMixed[] = {
        "s_swappc_b64 s[0:1], s[0:1]", "s_load_dwordx4 s[4:7], s[0:1], 0x0", "s_buffer_load_dword s0, s[8:11], 0x4",
        "s_waitcnt lgkmcnt(0)", "v_interp_p1_f32 v2, v0, attr0.y",
        "image_sample v[0:3], v2, s[4:11], s[8:11] dmask:0xf", "buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen",
        "v_mul_f32 v2, s0, v1", "v_cmp_lt_f32 vcc, v1, v8", "v_lshl_b32 v2, 0x3f000000, v1",
        "s_xor_b32 s0, s1, s2", "s_branch 18", "s_endpgm"};
    for (std::size_t n = 0; n < 13; ++n) {
        const auto text = gcnDisassemble(more.instructions[n]);
        if (text != expectedMixed[n]) { std::printf("%zu: %s\n", n, text.c_str()); assert(false); }
    }
    const std::uint32_t lanes[]={0x046b0e4f,0x02210935,0xd2040035,79u|(135u<<9),0xd2020010,309u|(132u<<9),0xbf810000};
    const auto laneOps=decodeGCN(lanes,sizeof lanes/4);
    assert(laneOps && laneOps.instructions.size()==5);
    assert(gcnDisassemble(laneOps.instructions[0])=="v_writelane_b32 v53, s79, 7");
    assert(gcnDisassemble(laneOps.instructions[1])=="v_readlane_b32 s16, v53, 4");
    assert(gcnDisassemble(laneOps.instructions[2])=="v_writelane_b32 v53, s79, 7");
    assert(gcnDisassemble(laneOps.instructions[3])=="v_readlane_b32 s16, v53, 4");
    const std::uint32_t relative[]={0x7e2c8704,0xd3860014,0x00000104,0xbf810000};
    const auto relativeOps=decodeGCN(relative,sizeof relative/4);
    assert(relativeOps && relativeOps.instructions.size()==3);
    assert(gcnDisassemble(relativeOps.instructions[0])=="v_movrels_b32 v22, v4");
    assert(gcnDisassemble(relativeOps.instructions[1])=="v_movrels_b32 v20, v4");
    // Truncation, missing s_endpgm and an unassigned encoding are refused.
    assert(decodeGCN(mixed, 5).error == GCNError::missing_endpgm);
    assert(decodeGCN(mixed + 5, 1).error == GCNError::truncated_instruction);
    const std::uint32_t bad[] = {0xfc000000};
    assert(decodeGCN(bad, 1).error == GCNError::unknown_encoding && decodeGCN(nullptr, 1).error == GCNError::null_input);
    std::puts("PASS GCN decoder: all encoding families, literals, embedded VS disassembly, OrbShdr program bounds");
}
