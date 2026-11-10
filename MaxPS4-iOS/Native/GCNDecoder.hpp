#pragma once
// Bounded GCN (Sea Islands / PS4 Liverpool) shader instruction decoder and
// disassembler for GPU shader research. It frames instructions and names
// their opcodes; it does not translate or execute shaders.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace maxps4::gpu {
enum class GCNEncoding : std::uint8_t {
    sop2, sopk, sop1, sopc, sopp, smrd, vop2, vop1, vopc, vop3, vintrp, ds, mubuf, mtbuf, mimg, exp
};
struct GCNInstruction {
    GCNEncoding encoding;
    std::uint16_t opcode;
    std::size_t word;         // index of the first dword
    std::uint8_t size;        // dwords, literal included
    std::uint32_t word0, word1; // word1 is the second dword of 64-bit encodings
    bool hasLiteral;
    std::uint32_t literal;
};
enum class GCNError { none, null_input, unknown_encoding, truncated_instruction, missing_endpgm, too_many_instructions };
struct GCNDecodeResult {
    GCNError error = GCNError::none;
    std::vector<GCNInstruction> instructions;
    explicit operator bool() const { return error == GCNError::none; }
};

namespace gcn_detail {
inline const char* pick(const char* const* table, std::size_t count, std::size_t op) {
    return op < count ? table[op] : nullptr;
}
#define MAXPS4_GCN_TABLE(name) inline const char* name(std::size_t op) { \
    static const char* const t[] = MAXPS4_GCN_ENTRIES; return pick(t, sizeof(t) / sizeof(t[0]), op); }
#define MAXPS4_GCN_ENTRIES { \
    "s_add_u32", "s_sub_u32", "s_add_i32", "s_sub_i32", "s_addc_u32", "s_subb_u32", "s_min_i32", "s_min_u32", \
    "s_max_i32", "s_max_u32", "s_cselect_b32", "s_cselect_b64", nullptr, nullptr, "s_and_b32", "s_and_b64", \
    "s_or_b32", "s_or_b64", "s_xor_b32", "s_xor_b64", "s_andn2_b32", "s_andn2_b64", "s_orn2_b32", "s_orn2_b64", \
    "s_nand_b32", "s_nand_b64", "s_nor_b32", "s_nor_b64", "s_xnor_b32", "s_xnor_b64", "s_lshl_b32", "s_lshl_b64", \
    "s_lshr_b32", "s_lshr_b64", "s_ashr_i32", "s_ashr_i64", "s_bfm_b32", "s_bfm_b64", "s_mul_i32", "s_bfe_u32", \
    "s_bfe_i32", "s_bfe_u64", "s_bfe_i64", "s_cbranch_g_fork", "s_absdiff_i32" }
MAXPS4_GCN_TABLE(sop2)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "s_movk_i32", nullptr, "s_cmovk_i32", "s_cmpk_eq_i32", "s_cmpk_lg_i32", "s_cmpk_gt_i32", "s_cmpk_ge_i32", \
    "s_cmpk_lt_i32", "s_cmpk_le_i32", "s_cmpk_eq_u32", "s_cmpk_lg_u32", "s_cmpk_gt_u32", "s_cmpk_ge_u32", \
    "s_cmpk_lt_u32", "s_cmpk_le_u32", "s_addk_i32", "s_mulk_i32", "s_cbranch_i_fork", "s_getreg_b32", \
    "s_setreg_b32", nullptr, "s_setreg_imm32_b32" }
MAXPS4_GCN_TABLE(sopk)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    nullptr, nullptr, nullptr, "s_mov_b32", "s_mov_b64", "s_cmov_b32", "s_cmov_b64", "s_not_b32", "s_not_b64", \
    "s_wqm_b32", "s_wqm_b64", "s_brev_b32", "s_brev_b64", "s_bcnt0_i32_b32", "s_bcnt0_i32_b64", \
    "s_bcnt1_i32_b32", "s_bcnt1_i32_b64", "s_ff0_i32_b32", "s_ff0_i32_b64", "s_ff1_i32_b32", "s_ff1_i32_b64", \
    "s_flbit_i32_b32", "s_flbit_i32_b64", "s_flbit_i32", "s_flbit_i32_i64", "s_sext_i32_i8", "s_sext_i32_i16", \
    "s_bitset0_b32", "s_bitset0_b64", "s_bitset1_b32", "s_bitset1_b64", "s_getpc_b64", "s_setpc_b64", \
    "s_swappc_b64", "s_rfe_b64", nullptr, "s_and_saveexec_b64", "s_or_saveexec_b64", "s_xor_saveexec_b64", \
    "s_andn2_saveexec_b64", "s_orn2_saveexec_b64", "s_nand_saveexec_b64", "s_nor_saveexec_b64", \
    "s_xnor_saveexec_b64", "s_quadmask_b32", "s_quadmask_b64", "s_movrels_b32", "s_movrels_b64", \
    "s_movreld_b32", "s_movreld_b64", "s_cbranch_join", nullptr, "s_abs_i32", "s_mov_fed_b32" }
MAXPS4_GCN_TABLE(sop1)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "s_cmp_eq_i32", "s_cmp_lg_i32", "s_cmp_gt_i32", "s_cmp_ge_i32", "s_cmp_lt_i32", "s_cmp_le_i32", \
    "s_cmp_eq_u32", "s_cmp_lg_u32", "s_cmp_gt_u32", "s_cmp_ge_u32", "s_cmp_lt_u32", "s_cmp_le_u32", \
    "s_bitcmp0_b32", "s_bitcmp1_b32", "s_bitcmp0_b64", "s_bitcmp1_b64", "s_setvskip" }
MAXPS4_GCN_TABLE(sopc)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "s_nop", "s_endpgm", "s_branch", nullptr, "s_cbranch_scc0", "s_cbranch_scc1", "s_cbranch_vccz", \
    "s_cbranch_vccnz", "s_cbranch_execz", "s_cbranch_execnz", "s_barrier", nullptr, "s_waitcnt", "s_sethalt", \
    "s_sleep", "s_setprio", "s_sendmsg", "s_sendmsghalt", "s_trap", "s_icache_inv", "s_incperflevel", \
    "s_decperflevel", "s_ttracedata", "s_cbranch_cdbgsys", "s_cbranch_cdbguser", "s_cbranch_cdbgsys_or_user", \
    "s_cbranch_cdbgsys_and_user" }
MAXPS4_GCN_TABLE(sopp)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "s_load_dword", "s_load_dwordx2", "s_load_dwordx4", "s_load_dwordx8", "s_load_dwordx16", nullptr, nullptr, \
    nullptr, "s_buffer_load_dword", "s_buffer_load_dwordx2", "s_buffer_load_dwordx4", "s_buffer_load_dwordx8", \
    "s_buffer_load_dwordx16", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, \
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "s_dcache_inv_vol", "s_memtime", "s_dcache_inv" }
MAXPS4_GCN_TABLE(smrd)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "v_cndmask_b32", "v_readlane_b32", "v_writelane_b32", "v_add_f32", "v_sub_f32", "v_subrev_f32", \
    "v_mac_legacy_f32", "v_mul_legacy_f32", "v_mul_f32", "v_mul_i32_i24", "v_mul_hi_i32_i24", "v_mul_u32_u24", \
    "v_mul_hi_u32_u24", "v_min_legacy_f32", "v_max_legacy_f32", "v_min_f32", "v_max_f32", "v_min_i32", \
    "v_max_i32", "v_min_u32", "v_max_u32", "v_lshr_b32", "v_lshrrev_b32", "v_ashr_i32", "v_ashrrev_i32", \
    "v_lshl_b32", "v_lshlrev_b32", "v_and_b32", "v_or_b32", "v_xor_b32", "v_bfm_b32", "v_mac_f32", \
    "v_madmk_f32", "v_madak_f32", "v_bcnt_u32_b32", "v_mbcnt_lo_u32_b32", "v_mbcnt_hi_u32_b32", "v_add_i32", \
    "v_sub_i32", "v_subrev_i32", "v_addc_u32", "v_subb_u32", "v_subbrev_u32", "v_ldexp_f32", \
    "v_cvt_pkaccum_u8_f32", "v_cvt_pknorm_i16_f32", "v_cvt_pknorm_u16_f32", "v_cvt_pkrtz_f16_f32", \
    "v_cvt_pk_u16_u32", "v_cvt_pk_i16_i32" }
MAXPS4_GCN_TABLE(vop2)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "v_nop", "v_mov_b32", "v_readfirstlane_b32", "v_cvt_i32_f64", "v_cvt_f64_i32", "v_cvt_f32_i32", \
    "v_cvt_f32_u32", "v_cvt_u32_f32", "v_cvt_i32_f32", "v_mov_fed_b32", "v_cvt_f16_f32", "v_cvt_f32_f16", \
    "v_cvt_rpi_i32_f32", "v_cvt_flr_i32_f32", "v_cvt_off_f32_i4", "v_cvt_f32_f64", "v_cvt_f64_f32", \
    "v_cvt_f32_ubyte0", "v_cvt_f32_ubyte1", "v_cvt_f32_ubyte2", "v_cvt_f32_ubyte3", "v_cvt_u32_f64", \
    "v_cvt_f64_u32", "v_trunc_f64", "v_ceil_f64", "v_rndne_f64", "v_floor_f64", nullptr, nullptr, nullptr, \
    nullptr, nullptr, "v_fract_f32", "v_trunc_f32", "v_ceil_f32", "v_rndne_f32", "v_floor_f32", "v_exp_f32", \
    "v_log_clamp_f32", "v_log_f32", "v_rcp_clamp_f32", "v_rcp_legacy_f32", "v_rcp_f32", "v_rcp_iflag_f32", \
    "v_rsq_clamp_f32", "v_rsq_legacy_f32", "v_rsq_f32", "v_rcp_f64", "v_rcp_clamp_f64", "v_rsq_f64", \
    "v_rsq_clamp_f64", "v_sqrt_f32", "v_sqrt_f64", "v_sin_f32", "v_cos_f32", "v_not_b32", "v_bfrev_b32", \
    "v_ffbh_u32", "v_ffbl_b32", "v_ffbh_i32", "v_frexp_exp_i32_f64", "v_frexp_mant_f64", "v_fract_f64", \
    "v_frexp_exp_i32_f32", "v_frexp_mant_f32", "v_clrexcp", "v_movreld_b32", "v_movrels_b32", "v_movrelsd_b32" }
MAXPS4_GCN_TABLE(vop1)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "v_mad_legacy_f32", "v_mad_f32", "v_mad_i32_i24", "v_mad_u32_u24", "v_cubeid_f32", "v_cubesc_f32", \
    "v_cubetc_f32", "v_cubema_f32", "v_bfe_u32", "v_bfe_i32", "v_bfi_b32", "v_fma_f32", "v_fma_f64", \
    "v_lerp_u8", "v_alignbit_b32", "v_alignbyte_b32", "v_mullit_f32", "v_min3_f32", "v_min3_i32", \
    "v_min3_u32", "v_max3_f32", "v_max3_i32", "v_max3_u32", "v_med3_f32", "v_med3_i32", "v_med3_u32", \
    "v_sad_u8", "v_sad_hi_u8", "v_sad_u16", "v_sad_u32", "v_cvt_pk_u8_f32", "v_div_fixup_f32", \
    "v_div_fixup_f64", "v_lshl_b64", "v_lshr_b64", "v_ashr_i64", "v_add_f64", "v_mul_f64", "v_min_f64", \
    "v_max_f64", "v_ldexp_f64", "v_mul_lo_u32", "v_mul_hi_u32", "v_mul_lo_i32", "v_mul_hi_i32", \
    "v_div_scale_f32", "v_div_scale_f64", "v_div_fmas_f32", "v_div_fmas_f64", "v_msad_u8", \
    "v_qsad_pk_u16_u8", "v_mqsad_pk_u16_u8", "v_trig_preop_f64", "v_mqsad_u32_u8", "v_mad_u64_u32", \
    "v_mad_i64_i32" }
MAXPS4_GCN_TABLE(vop3only) // VOP3 opcodes 320 and up
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { "v_interp_p1_f32", "v_interp_p2_f32", "v_interp_mov_f32" }
MAXPS4_GCN_TABLE(vintrp)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "buffer_load_format_x", "buffer_load_format_xy", "buffer_load_format_xyz", "buffer_load_format_xyzw", \
    "buffer_store_format_x", "buffer_store_format_xy", "buffer_store_format_xyz", "buffer_store_format_xyzw", \
    "buffer_load_ubyte", "buffer_load_sbyte", "buffer_load_ushort", "buffer_load_sshort", "buffer_load_dword", \
    "buffer_load_dwordx2", "buffer_load_dwordx4", "buffer_load_dwordx3", nullptr, nullptr, nullptr, nullptr, \
    nullptr, nullptr, nullptr, nullptr, "buffer_store_byte", nullptr, "buffer_store_short", nullptr, \
    "buffer_store_dword", "buffer_store_dwordx2", "buffer_store_dwordx4", "buffer_store_dwordx3" }
MAXPS4_GCN_TABLE(mubuf)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "tbuffer_load_format_x", "tbuffer_load_format_xy", "tbuffer_load_format_xyz", "tbuffer_load_format_xyzw", \
    "tbuffer_store_format_x", "tbuffer_store_format_xy", "tbuffer_store_format_xyz", "tbuffer_store_format_xyzw" }
MAXPS4_GCN_TABLE(mtbuf)
#undef MAXPS4_GCN_ENTRIES
#define MAXPS4_GCN_ENTRIES { \
    "image_load", "image_load_mip", "image_load_pck", "image_load_pck_sgn", "image_load_mip_pck", \
    "image_load_mip_pck_sgn", nullptr, nullptr, "image_store", "image_store_mip", "image_store_pck", \
    "image_store_mip_pck", nullptr, nullptr, "image_get_resinfo", nullptr, nullptr, nullptr, nullptr, nullptr, \
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, \
    "image_sample", "image_sample_cl", "image_sample_d", "image_sample_d_cl", "image_sample_l", \
    "image_sample_b", "image_sample_b_cl", "image_sample_lz", "image_sample_c", "image_sample_c_cl", \
    "image_sample_c_d", "image_sample_c_d_cl", "image_sample_c_l", "image_sample_c_b", "image_sample_c_b_cl", \
    "image_sample_c_lz", "image_sample_o", "image_sample_cl_o", "image_sample_d_o", "image_sample_d_cl_o", \
    "image_sample_l_o", "image_sample_b_o", "image_sample_b_cl_o", "image_sample_lz_o", "image_sample_c_o", \
    "image_sample_c_cl_o", "image_sample_c_d_o", "image_sample_c_d_cl_o", "image_sample_c_l_o", \
    "image_sample_c_b_o", "image_sample_c_b_cl_o", "image_sample_c_lz_o", "image_gather4", "image_gather4_cl", nullptr, nullptr, \
    "image_gather4_l", "image_gather4_b", "image_gather4_b_cl", "image_gather4_lz", "image_gather4_c", \
    "image_gather4_c_cl", nullptr, nullptr, "image_gather4_c_l", "image_gather4_c_b", "image_gather4_c_b_cl", "image_gather4_c_lz", \
    "image_gather4_o", "image_gather4_cl_o", nullptr, nullptr, "image_gather4_l_o", "image_gather4_b_o", "image_gather4_b_cl_o", \
    "image_gather4_lz_o", "image_gather4_c_o", "image_gather4_c_cl_o", nullptr, nullptr, "image_gather4_c_l_o", \
    "image_gather4_c_b_o", "image_gather4_c_b_cl_o", "image_gather4_c_lz_o", "image_get_lod" }
MAXPS4_GCN_TABLE(mimg)
#undef MAXPS4_GCN_ENTRIES
#undef MAXPS4_GCN_TABLE

// VOPC: opcode bits [7:4] select the type/x family, [3:0] the condition.
inline std::string vopc(std::size_t op) {
    static const char* const fcond[] = {"f", "lt", "eq", "le", "gt", "lg", "ge", "o", "u", "nge", "nlg", "ngt",
                                        "nle", "neq", "nlt", "tru"};
    static const char* const icond[] = {"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};
    static const char* const ffamily[] = {"cmp", "cmpx", "cmp", "cmpx", "cmps", "cmpsx", "cmps", "cmpsx"};
    if (op > 0xff) return {};
    const std::size_t family = op >> 4, cond = op & 15;
    if (family < 8) return std::string("v_") + ffamily[family] + "_" + fcond[cond] + (family & 2 ? "_f64" : "_f32");
    static const char* const itype[] = {"i32", "i32", "i64", "i64", "u32", "u32", "u64", "u64"};
    const bool x = family & 1;
    if (cond >= 8) {
        if (cond != 8 || family > 11) return {};
        return std::string("v_") + (x ? "cmpx" : "cmp") + (family & 2 ? "_class_f64" : "_class_f32");
    }
    return std::string("v_") + (x ? "cmpx_" : "cmp_") + icond[cond] + "_" + itype[family - 8];
}
} // namespace gcn_detail

// Instruction name, or an empty string for an unassigned opcode.
inline std::string gcnMnemonic(const GCNInstruction& i) {
    using namespace gcn_detail;
    const char* name = nullptr;
    switch (i.encoding) {
    case GCNEncoding::sop2: name = sop2(i.opcode); break;
    case GCNEncoding::sopk: name = sopk(i.opcode); break;
    case GCNEncoding::sop1: name = sop1(i.opcode); break;
    case GCNEncoding::sopc: name = sopc(i.opcode); break;
    case GCNEncoding::sopp: name = sopp(i.opcode); break;
    case GCNEncoding::smrd: name = smrd(i.opcode); break;
    case GCNEncoding::vop2: name = vop2(i.opcode); break;
    case GCNEncoding::vop1: name = vop1(i.opcode); break;
    case GCNEncoding::vopc: return vopc(i.opcode);
    case GCNEncoding::vop3:
        // VOP3 encodes VOPC (0-255), VOP2 (256-319), its own (320-383), VOP1 (384+).
        if (i.opcode < 256) return vopc(i.opcode);
        if (i.opcode < 320) name = vop2(i.opcode - 256);
        else if (i.opcode < 384) name = vop3only(i.opcode - 320);
        else name = vop1(i.opcode - 384);
        break;
    case GCNEncoding::vintrp: name = vintrp(i.opcode); break;
    // Only the LDS-free cross-lane swizzle is named (derivatives use it).
    case GCNEncoding::ds: return i.opcode == 53 ? "ds_swizzle_b32" : "ds_op" + std::to_string(i.opcode);
    case GCNEncoding::mubuf: name = mubuf(i.opcode); break;
    case GCNEncoding::mtbuf: name = mtbuf(i.opcode); break;
    case GCNEncoding::mimg: name = mimg(i.opcode); break;
    case GCNEncoding::exp: return "exp";
    }
    return name ? name : "";
}

// Decode the one instruction at words[at]; `available` dwords follow it.
inline GCNError decodeGCNInstruction(const std::uint32_t* words, std::size_t available, std::size_t at,
                                     GCNInstruction& i) {
    if (!words || !available) return GCNError::null_input;
    const std::uint32_t w = words[0];
    i = GCNInstruction{GCNEncoding::sop2, 0, at, 1, w, 0, false, 0};
    bool wide = false;
    auto src9 = [](std::uint32_t v) { return (v & 0x1ff) == 255; };
    if ((w >> 23) == 0x17d) { i.encoding = GCNEncoding::sop1; i.opcode = (w >> 8) & 0xff; i.hasLiteral = (w & 0xff) == 255; }
    else if ((w >> 23) == 0x17e) { i.encoding = GCNEncoding::sopc; i.opcode = (w >> 16) & 0x7f; i.hasLiteral = (w & 0xff) == 255 || ((w >> 8) & 0xff) == 255; }
    else if ((w >> 23) == 0x17f) { i.encoding = GCNEncoding::sopp; i.opcode = (w >> 16) & 0x7f; }
    else if ((w >> 28) == 0xb) { i.encoding = GCNEncoding::sopk; i.opcode = (w >> 23) & 0x1f; i.hasLiteral = i.opcode == 21; }
    else if ((w >> 30) == 2) { i.encoding = GCNEncoding::sop2; i.opcode = (w >> 23) & 0x7f; i.hasLiteral = (w & 0xff) == 255 || ((w >> 8) & 0xff) == 255; }
    // SMRD: IMM=0 with OFFSET=255 takes a 32-bit literal offset (Sea Islands).
    else if ((w >> 27) == 0x18) { i.encoding = GCNEncoding::smrd; i.opcode = (w >> 22) & 0x1f; i.hasLiteral = !(w & 0x100) && (w & 0xff) == 255; }
    else if ((w >> 25) == 0x3f) { i.encoding = GCNEncoding::vop1; i.opcode = (w >> 9) & 0xff; i.hasLiteral = src9(w); }
    else if ((w >> 25) == 0x3e) { i.encoding = GCNEncoding::vopc; i.opcode = (w >> 17) & 0xff; i.hasLiteral = src9(w); }
    else if ((w >> 31) == 0) {
        i.encoding = GCNEncoding::vop2; i.opcode = (w >> 25) & 0x3f;
        i.hasLiteral = src9(w) || i.opcode == 32 || i.opcode == 33; // v_madmk/v_madak
    }
    else if ((w >> 26) == 0x34) { i.encoding = GCNEncoding::vop3; i.opcode = (w >> 17) & 0x1ff; wide = true; }
    else if ((w >> 26) == 0x32) { i.encoding = GCNEncoding::vintrp; i.opcode = (w >> 16) & 3; }
    else if ((w >> 26) == 0x36) { i.encoding = GCNEncoding::ds; i.opcode = (w >> 18) & 0xff; wide = true; }
    else if ((w >> 26) == 0x38) { i.encoding = GCNEncoding::mubuf; i.opcode = (w >> 18) & 0x7f; wide = true; }
    else if ((w >> 26) == 0x3a) { i.encoding = GCNEncoding::mtbuf; i.opcode = (w >> 16) & 7; wide = true; }
    else if ((w >> 26) == 0x3c) { i.encoding = GCNEncoding::mimg; i.opcode = (w >> 18) & 0x7f; wide = true; }
    else if ((w >> 26) == 0x3e) { i.encoding = GCNEncoding::exp; wide = true; }
    else return GCNError::unknown_encoding;
    i.size = std::uint8_t(1 + (wide ? 1 : 0) + (i.hasLiteral ? 1 : 0));
    if (i.size > available) return GCNError::truncated_instruction;
    if (wide) i.word1 = words[1];
    if (i.hasLiteral) i.literal = words[wide ? 2 : 1];
    return GCNError::none;
}

// Frame instructions from `words` up to and including the first s_endpgm.
inline GCNDecodeResult decodeGCN(const std::uint32_t* words, std::size_t wordCount,
                                 std::size_t maxInstructions = 16384) {
    GCNDecodeResult result;
    if (wordCount && !words) { result.error = GCNError::null_input; return result; }
    auto fail = [&](GCNError error) { result.error = error; result.instructions.clear(); return result; };
    for (std::size_t at = 0; at < wordCount;) {
        if (result.instructions.size() >= maxInstructions) return fail(GCNError::too_many_instructions);
        GCNInstruction i;
        if (const auto error = decodeGCNInstruction(words + at, wordCount - at, at, i); error != GCNError::none)
            return fail(error);
        result.instructions.push_back(i);
        at += i.size;
        if (i.encoding == GCNEncoding::sopp && i.opcode == 1) return result;
    }
    return fail(GCNError::missing_endpgm);
}

namespace gcn_detail {
inline std::string hex(std::uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "0x%x", v); return b; }
inline std::string range(char kind, unsigned base, unsigned count) {
    if (count <= 1) return kind + std::to_string(base);
    return std::string(1, kind) + "[" + std::to_string(base) + ":" + std::to_string(base + count - 1) + "]";
}
// A 9-bit source operand (scalar for values below 256), `count` registers wide.
inline std::string operand(std::uint32_t v, const GCNInstruction& i, unsigned count = 1) {
    static const char* const floats[] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"};
    if (v >= 256) return range('v', v - 256, count);
    if (v <= 103) return range('s', v, count);
    if (v == 106) return count == 2 ? "vcc" : "vcc_lo";
    if (v == 107) return "vcc_hi";
    if (v >= 112 && v <= 123) return "ttmp" + std::to_string(v - 112);
    if (v == 124) return "m0";
    if (v == 126) return count == 2 ? "exec" : "exec_lo";
    if (v == 127) return "exec_hi";
    if (v == 128) return "0";
    if (v >= 129 && v <= 192) return std::to_string(v - 128);
    if (v >= 193 && v <= 208) return std::to_string(192 - int(v));
    if (v >= 240 && v <= 247) return floats[v - 240];
    if (v == 251) return "vccz";
    if (v == 252) return "execz";
    if (v == 253) return "scc";
    if (v == 254) return "lds_direct";
    if (v == 255) return hex(i.literal);
    return "src" + std::to_string(v);
}
inline bool endsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
} // namespace gcn_detail

// One line of disassembly. Vector operands show their first register only.
inline std::string gcnDisassemble(const GCNInstruction& i) {
    using namespace gcn_detail;
    std::string name = gcnMnemonic(i);
    if (name.empty()) {
        static const char* const encodings[] = {"sop2", "sopk", "sop1", "sopc", "sopp", "smrd", "vop2", "vop1",
                                                "vopc", "vop3", "vintrp", "ds", "mubuf", "mtbuf", "mimg", "exp"};
        name = std::string(encodings[unsigned(i.encoding)]) + "_op" + std::to_string(i.opcode);
    }
    const std::uint32_t w = i.word0, w1 = i.word1;
    std::string s = name;
    auto arg = [&](const std::string& a) { s += (s.size() == name.size() ? " " : ", ") + a; };
    const bool wide64 = endsWith(name, "_b64") || endsWith(name, "_i64") || endsWith(name, "_u64");
    switch (i.encoding) {
    case GCNEncoding::sop2: {
        // Shift and field amounts are 32-bit; s_bfm_b64 takes two 32-bit sources.
        const bool narrow1 = name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" ||
                             name == "s_bfe_u64" || name == "s_bfe_i64" || name == "s_bfm_b64";
        const unsigned width = wide64 ? 2 : 1;
        arg(operand((w >> 16) & 0x7f, i, width));
        arg(operand(w & 0xff, i, name == "s_bfm_b64" ? 1 : width));
        arg(operand((w >> 8) & 0xff, i, narrow1 ? 1 : width));
        break;
    }
    case GCNEncoding::sopk:
        arg(operand((w >> 16) & 0x7f, i));
        arg(hex(w & 0xffff));
        if (i.hasLiteral) arg(hex(i.literal));
        break;
    case GCNEncoding::sop1: {
        const bool noDst = name == "s_setpc_b64" || name == "s_cbranch_join";
        const bool noSrc = name == "s_getpc_b64";
        // s_bcnt*/s_ff*/s_flbit*_b64 produce a 32-bit result from a 64-bit source.
        const bool narrowDst = name.find("_i32_") != std::string::npos;
        if (!noDst) arg(operand((w >> 16) & 0x7f, i, wide64 && !narrowDst ? 2 : 1));
        if (!noSrc) arg(operand(w & 0xff, i, wide64 ? 2 : 1));
        break;
    }
    case GCNEncoding::sopc: {
        const unsigned width = endsWith(name, "_b64") ? 2 : 1;
        arg(operand(w & 0xff, i, width));
        arg(operand((w >> 8) & 0xff, i, 1));
        break;
    }
    case GCNEncoding::sopp: {
        const std::uint32_t imm = w & 0xffff;
        if (i.opcode == 12) { // s_waitcnt: vmcnt [3:0], expcnt [6:4], lgkmcnt [12:8]
            if ((imm & 15) != 15) s += " vmcnt(" + std::to_string(imm & 15) + ")";
            if (((imm >> 4) & 7) != 7) s += " expcnt(" + std::to_string((imm >> 4) & 7) + ")";
            if (((imm >> 8) & 31) != 31) s += " lgkmcnt(" + std::to_string((imm >> 8) & 31) + ")";
        } else if (i.opcode == 2 || (i.opcode >= 4 && i.opcode <= 9)) {
            s += " " + std::to_string(long(i.word) + 1 + std::int16_t(imm)); // target dword
        } else if (imm || (i.opcode != 1 && i.opcode != 10)) {
            s += " " + hex(imm);
        }
        break;
    }
    case GCNEncoding::smrd: {
        static const unsigned counts[] = {1, 2, 4, 8, 16};
        const unsigned count = counts[(i.opcode & 7) < 5 ? (i.opcode & 7) : 0];
        if (i.opcode >= 29) { if (i.opcode == 30) arg(range('s', (w >> 15) & 0x7f, 2)); break; }
        arg(operand((w >> 15) & 0x7f, i, count));
        arg(range('s', ((w >> 9) & 0x3f) * 2, i.opcode >= 8 ? 4 : 2));
        arg((w & 0x100) ? hex(w & 0xff) : i.hasLiteral ? hex(i.literal) : operand(w & 0xff, i));
        break;
    }
    case GCNEncoding::vop2:
        arg(operand(256 + ((w >> 17) & 0xff), i));
        if (i.opcode >= 37 && i.opcode <= 42) arg("vcc"); // carry out
        arg(operand(w & 0x1ff, i));
        if (i.opcode == 32) arg(hex(i.literal));          // v_madmk_f32 d, s0, K, s1
        arg(operand(256 + ((w >> 9) & 0xff), i));
        if (i.opcode == 0 || (i.opcode >= 40 && i.opcode <= 42)) arg("vcc"); // carry/select in
        if (i.opcode == 33) arg(hex(i.literal));          // v_madak_f32 d, s0, s1, K
        break;
    case GCNEncoding::vop1:
        if (i.opcode == 0 || i.opcode == 65) break; // v_nop, v_clrexcp
        arg(i.opcode == 2 ? operand((w >> 17) & 0xff, i) : operand(256 + ((w >> 17) & 0xff), i));
        arg(operand(w & 0x1ff, i));
        break;
    case GCNEncoding::vopc:
        arg("vcc");
        arg(operand(w & 0x1ff, i));
        arg(operand(256 + ((w >> 9) & 0xff), i));
        break;
    case GCNEncoding::vop3: {
        const unsigned op = i.opcode;
        const unsigned abs = (w >> 8) & 7, neg = w1 >> 29;
        // VOP3b (carry and div_scale) uses [14:8] as the scalar destination.
        const bool vop3b = (op >= 293 && op <= 298) || op == 365 || op == 366;
        auto src = [&](unsigned n) {
            std::string a = operand((w1 >> (9 * n)) & 0x1ff, i);
            if (!vop3b && (abs >> n) & 1) a = "|" + a + "|";
            if ((neg >> n) & 1) a = "-" + a;
            return a;
        };
        unsigned sources = 2;
        if (op < 256) arg(operand(w & 0xff, i, 2)); // compare: SGPR pair destination
        else arg(operand(256 + (w & 0xff), i));
        if (op >= 320 && op < 384) sources = 3;
        else if (op >= 384) sources = 1;
        else if (op == 256 || op == 296 || op == 297 || op == 298) sources = 3; // cndmask, addc/subb
        if (vop3b) arg(operand((w >> 8) & 0x7f, i, 2));
        for (unsigned n = 0; n < sources; ++n) arg(src(n));
        if (w & 0x800) s += " clamp";
        static const char* const omods[] = {"", " mul:2", " mul:4", " div:2"};
        s += omods[(w1 >> 27) & 3];
        break;
    }
    case GCNEncoding::vintrp: {
        static const char* const chans = "xyzw";
        arg(operand(256 + ((w >> 18) & 0xff), i));
        if (i.opcode == 2) { static const char* const p[] = {"p10", "p20", "p0", "p?"}; arg(p[w & 3]); }
        else arg(operand(256 + (w & 0xff), i));
        arg("attr" + std::to_string((w >> 10) & 0x3f) + "." + chans[(w >> 8) & 3]);
        break;
    }
    case GCNEncoding::ds:
        arg(operand(256 + ((w1 >> 24) & 0xff), i));
        arg(operand(256 + (w1 & 0xff), i));
        arg(operand(256 + ((w1 >> 8) & 0xff), i));
        s += " offset0:" + std::to_string(w & 0xff) + " offset1:" + std::to_string((w >> 8) & 0xff);
        break;
    case GCNEncoding::mubuf:
    case GCNEncoding::mtbuf: {
        const bool typed = i.encoding == GCNEncoding::mtbuf;
        unsigned count = 1;
        if (endsWith(name, "_xy") || endsWith(name, "x2")) count = 2;
        else if (endsWith(name, "_xyz") || endsWith(name, "x3")) count = 3;
        else if (endsWith(name, "_xyzw") || endsWith(name, "x4")) count = 4;
        arg(range('v', (w1 >> 8) & 0xff, count));
        arg(operand(256 + (w1 & 0xff), i));
        arg(range('s', ((w1 >> 16) & 0x1f) * 4, 4));
        arg(operand(w1 >> 24, i));
        if (w & 0x2000) s += " idxen";
        if (w & 0x1000) s += " offen";
        if (w & 0x8000) s += " addr64";
        if (w & 0xfff) s += " offset:" + std::to_string(w & 0xfff);
        if (typed) s += " dfmt:" + std::to_string((w >> 19) & 15) + " nfmt:" + std::to_string((w >> 23) & 7);
        if (w & 0x4000) s += " glc";
        if (w1 & 0x400000) s += " slc";
        break;
    }
    case GCNEncoding::mimg: {
        const unsigned dmask = (w >> 8) & 15;
        unsigned count = 0;
        for (unsigned b = 0; b < 4; ++b) count += (dmask >> b) & 1;
        arg(range('v', (w1 >> 8) & 0xff, count ? count : 1));
        arg(operand(256 + (w1 & 0xff), i));
        arg(range('s', ((w1 >> 16) & 0x1f) * 4, (w & 0x8000) ? 4 : 8));
        if (name.compare(0, 12, "image_sample") == 0 || name.compare(0, 12, "image_gather") == 0 || name == "image_get_lod")
            arg(range('s', ((w1 >> 21) & 0x1f) * 4, 4));
        s += " dmask:" + hex(dmask);
        if (w & 0x1000) s += " unorm";
        if (w & 0x4000) s += " da";
        if (w & 0x2000) s += " glc";
        break;
    }
    case GCNEncoding::exp: {
        const unsigned target = (w >> 4) & 0x3f;
        std::string t;
        if (target < 8) t = "mrt" + std::to_string(target);
        else if (target == 8) t = "mrtz";
        else if (target == 9) t = "null";
        else if (target >= 12 && target < 16) t = "pos" + std::to_string(target - 12);
        else if (target >= 32 && target < 64) t = "param" + std::to_string(target - 32);
        else t = "target" + std::to_string(target);
        arg(t);
        for (unsigned n = 0; n < 4; ++n) arg((w >> n) & 1 ? operand(256 + ((w1 >> (8 * n)) & 0xff), i) : "off");
        if (w & 0x400) s += " compr";
        if (w & 0x800) s += " done";
        if (w & 0x1000) s += " vm";
        break;
    }
    }
    return s;
}

// A Sony-compiled program starts with `s_mov_b32 vcc_hi, N`; its "OrbShdr"
// binary information follows at dword (N + 1) * 2 and records the code length.
struct GCNProgram {
    std::size_t codeWords = 0; // code dwords from the program start
    std::uint8_t type = 0;     // binary information shader type field
};
inline bool locateGCNProgram(const std::uint32_t* words, std::size_t wordCount, GCNProgram& program) {
    if (!words || wordCount < 2 || words[0] != 0xbeeb03ffU) return false;
    const std::size_t info = (std::size_t(words[1]) + 1) * 2;
    if (words[1] >= 0x10000 || info > wordCount || wordCount - info < 3) return false;
    if (words[info] != 0x5362724fU || (words[info + 1] & 0xffffff) != 0x726468U) return false; // "OrbShdr"
    const std::uint32_t bits = words[info + 2];
    const std::size_t length = bits >> 8;
    if ((length & 3) || length / 4 > info) return false;
    program.codeWords = length / 4;
    program.type = std::uint8_t((bits >> 2) & 15);
    return true;
}
} // namespace maxps4::gpu
