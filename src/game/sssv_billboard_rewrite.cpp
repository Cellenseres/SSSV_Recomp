#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "recomp.h"
#include "rt64_extended_gbi.h"
#include "sssv_billboard_controls.h"

// Debug logging: on in debug builds, off in release builds.
#ifndef SSSV_BILLBOARD_DEBUG
	#if !defined(NDEBUG)
		#define SSSV_BILLBOARD_DEBUG 1
	#else
		#define SSSV_BILLBOARD_DEBUG 0
	#endif
#endif

namespace {

constexpr gpr vram32(uint32_t v) {
	return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(v)));
}

constexpr gpr ADDR_D_80204278_PTR = vram32(0x80204278);
constexpr gpr ADDR_SCREEN_WIDTH   = vram32(0x80203FD0);
constexpr gpr ADDR_SCREEN_HEIGHT  = vram32(0x80203FD2);
constexpr gpr ADDR_LEVEL_CONFIG   = vram32(0x803F2D50);

constexpr int LEVELCFG_OFF_FOV_Y          = 0xE0;
constexpr int LEVELCFG_OFF_PRIMDEPTH_BIAS = 0x42;

constexpr int DISPLAYLIST_OFF_VIEWPROJ_F32 = 0x38A10;

// Billboard data pool in extended RDRAM.
// The game's original vertex pool (dl_state + 0x2C570) is only 1000 slots (16 KB)
// shared with other game data. Instead, we allocate our own pool in extended RDRAM
// (addresses >= 0x80800000). The recomp framework allocates 512 MB, so this is safe.
// RT64's gEXSetRDRAMExtended (which we already emit) handles these addresses.
constexpr uint32_t BILLBOARD_POOL_VRAM  = 0x80900000u;
constexpr int      BILLBOARD_POOL_SLOTS = 8192;  // 8192 * 16 = 128 KB
constexpr int      BILLBOARD_SLOT_BYTES = 16;

constexpr uint32_t CMD_SETPRIMDEPTH = 0xEE000000;
constexpr uint32_t CMD_TRI2         = 0xB1000000; // F3DEX G_TRI2
constexpr uint32_t CMD_POPMTX       = 0xBD000000; // F3D/F3DEX G_POPMTX

// F3D/F3DEX G_SETOTHERMODE_H: set 1 bit at shift 19 (G_MDSFT_TEXTPERSP) to G_TP_PERSP
// so RT64 does not apply the 0.5 UV correction (which would show only top-left quarter).
constexpr uint32_t CMD_SETOTHERMODE_H_TP_PERSP    = 0xBA130001u; // size=1, shift=19
constexpr uint32_t CMD_SETOTHERMODE_H_TP_PERSP_W1 = 0x00080000u; // G_TP_PERSP = (1<<19)

// gEXMatrixFloat params for F3DEX (pre-XORed with pushMask=0x04).
// The gEXMatrixFloat handler XORs the param byte with pushMask before
// passing it to matrixCommon, so we pre-XOR to get the desired flags:
//   projMask=0x01, loadMask=0x02, pushMask=0x04
constexpr uint8_t GEXMTX_LOAD_PROJ           = 0x07; // => 0x03: LOAD | PROJECTION
constexpr uint8_t GEXMTX_PUSH_LOAD_MODELVIEW = 0x02; // => 0x06: PUSH | LOAD | MODELVIEW

// RDRAM in N64 is 8 MiB.
constexpr uint32_t RDRAM_SIZE_BYTES = 0x0080'0000u;

// Cull threshold for "behind camera". Kept as a named constant for clarity/tuning.
constexpr float kBehindCameraZ = -3.0f;

bool g_disable_6fa3a4_render = false;
bool g_disable_6c5e44_render = false;
bool g_disable_73f17c_render = false;
bool g_disable_73f800_render = false;
bool g_disable_740094_render = false;
bool g_disable_740820_render = false;
bool g_rewrite_6c5e44_ortho  = true;
bool g_rewrite_73f17c_ortho  = true;
bool g_rewrite_73f800_ortho  = true;
bool g_rewrite_740094_ortho  = true;
bool g_rewrite_740820_ortho  = true;
#if defined(NDEBUG)
bool g_rewrite_6c5e44_suppress_original = true;
bool g_rewrite_73f17c_suppress_original = true;  // Release: Hide Original On
bool g_rewrite_73f800_suppress_original = true;
bool g_rewrite_740094_suppress_original = true;
bool g_rewrite_740820_suppress_original = true;
bool g_log_73f17c_ortho = false;                 // Release: Ortho Logs Off
#else
bool g_rewrite_6c5e44_suppress_original = false;
bool g_rewrite_73f17c_suppress_original = false;
bool g_rewrite_73f800_suppress_original = false;
bool g_rewrite_740094_suppress_original = false;
bool g_rewrite_740820_suppress_original = false;
bool g_log_73f17c_ortho = true;
#endif
uint64_t g_billboard_frame_count = 0;

enum class RewriteOutcome {
	Emitted = 0,
	InvalidArgs,
	InvalidScreen,
	MissingDlState,
	BehindCamera,
	InvalidClipW,
	InvalidFov,
	InvalidSpriteScale,
	Offscreen,
	AllocFail,
	GfxPtrFail,
	GfxCapacityFail
};

struct RewriteTrace {
	int32_t world_x = 0;
	int32_t world_y = 0;
	int32_t world_z = 0;
	int16_t half_w = 0;
	int16_t half_h = 0;
	int32_t scale = 0;
	int16_t screen_w = 0;
	int16_t screen_h = 0;
	float cam_z = 0.0f;
	float clip_w = 0.0f;
	float sprite_scale = 0.0f;
	float xl = 0.0f;
	float yl = 0.0f;
	float xh = 0.0f;
	float yh = 0.0f;
	uint32_t group_id = 0;
};

static const char* rewrite_outcome_name(RewriteOutcome outcome) {
	switch (outcome) {
		case RewriteOutcome::Emitted: return "emitted";
		case RewriteOutcome::InvalidArgs: return "invalid_args";
		case RewriteOutcome::InvalidScreen: return "invalid_screen";
		case RewriteOutcome::MissingDlState: return "missing_dl_state";
		case RewriteOutcome::BehindCamera: return "behind_camera";
		case RewriteOutcome::InvalidClipW: return "invalid_clip_w";
		case RewriteOutcome::InvalidFov: return "invalid_fov";
		case RewriteOutcome::InvalidSpriteScale: return "invalid_sprite_scale";
		case RewriteOutcome::Offscreen: return "offscreen";
		case RewriteOutcome::AllocFail: return "alloc_fail";
		case RewriteOutcome::GfxPtrFail: return "gfx_ptr_fail";
		case RewriteOutcome::GfxCapacityFail: return "gfx_capacity_fail";
		default: return "unknown";
	}
}

struct Rt64VertexColor {
	int16_t y;
	int16_t x;
	uint16_t flag;
	int16_t z;
	int16_t t;
	int16_t s;
	uint8_t a;
	uint8_t b;
	uint8_t g;
	uint8_t r;
};

struct Rt64VertexExV1 {
	Rt64VertexColor v;
	int16_t yp;
	int16_t xp;
	uint16_t pad;
	int16_t zp;
};

static_assert(sizeof(Rt64VertexExV1) == 24, "Unexpected Rt64VertexExV1 size");

// Cache of previous quad screen-space positions for interpolation.
struct PrevQuad {
	int16_t x[4];
	int16_t y[4];
	// Optional signature to reduce rare hash-collision artifacts.
	int32_t sig_x;
	int32_t sig_y;
	int32_t sig_z;
	uint64_t stamp;
};

std::unordered_map<uint32_t, PrevQuad> g_prev_quads;
uint64_t g_quad_stamp = 0;

inline uint32_t vram_to_phys_u32(gpr vram_addr) {
	return static_cast<uint32_t>(static_cast<int32_t>(vram_addr) - static_cast<int32_t>(0x80000000u));
}

inline float read_f32(uint8_t* rdram, gpr addr) {
	const uint32_t bits = static_cast<uint32_t>(MEM_W(0, addr));
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof(value));
	(void)rdram;
	return value;
}

inline int16_t clamp_i16(float value) {
	const int rounded = static_cast<int>(std::lround(value));
	return static_cast<int16_t>(std::clamp(rounded, -32768, 32767));
}

inline uint32_t hash_u32(uint32_t hash, uint32_t value) {
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

inline uint32_t billboard_group_id(int32_t x, int32_t y, int32_t z, int16_t w, int16_t h, int32_t s, uint32_t salt) {
	// FNV-1a style mixing.
	uint32_t hash = 2166136261u;
	hash = hash_u32(hash, static_cast<uint32_t>(x));
	hash = hash_u32(hash, static_cast<uint32_t>(y));
	hash = hash_u32(hash, static_cast<uint32_t>(z));
	hash = hash_u32(hash, static_cast<uint16_t>(w));
	hash = hash_u32(hash, static_cast<uint16_t>(h));
	hash = hash_u32(hash, static_cast<uint32_t>(s));
	hash ^= salt;
	return (hash == 0) ? 1u : hash;
}

struct GfxWriteContext {
	gpr gdl_vram = 0;          // current write pointer (vram)
	uint32_t gdl_phys = 0;     // current write pointer (phys)
	uint8_t* gfx_mem = nullptr;
	GfxCommand* cmd = nullptr;
	uint32_t capacity_bytes = 0;
};

// Reads the current Gfx pointer from *vram_ptr, validates it, and returns a writable command pointer.
// Also performs an optional capacity check for a known maximum RDRAM size.
static bool try_get_gfx_ptr(uint8_t* rdram, gpr vram_ptr, GfxWriteContext& out) {
	const gpr gdl = MEM_W(0, vram_ptr);
	if (gdl == 0) {
		return false;
	}

	const uint32_t phys = vram_to_phys_u32(gdl);
	if (phys >= RDRAM_SIZE_BYTES) {
		return false;
	}

	out.gdl_vram = gdl;
	out.gdl_phys = phys;
	out.gfx_mem = rdram + phys;
	out.cmd = reinterpret_cast<GfxCommand*>(out.gfx_mem);
	out.capacity_bytes = RDRAM_SIZE_BYTES - phys;
	(void)rdram;
	return true;
}

static void advance_gfx_ptr(uint8_t* rdram, const GfxWriteContext& wctx, GfxCommand* end_cmd, gpr vram_ptr) {
	// end_cmd is in the same region starting at wctx.gfx_mem.
	const ptrdiff_t written = reinterpret_cast<uint8_t*>(end_cmd) - wctx.gfx_mem;
	const gpr new_gdl = ADD32(wctx.gdl_vram, static_cast<gpr>(written));
	MEM_W(0, vram_ptr) = static_cast<int32_t>(new_gdl);
	(void)rdram;
}

// Per-frame billboard allocator in extended RDRAM with integrated matrix cache.
struct BillboardAllocator {
	gpr dl_state = 0;            // frame detection: new dl_state = new frame
	int32_t used_slots = 0;      // linear allocation counter, reset each frame
	// Ortho/identity matrix cache in extended RDRAM (shared across all billboard types within a frame)
	gpr proj_mtx_addr = 0;
	gpr view_mtx_addr = 0;
	int16_t screen_w = 0;
	int16_t screen_h = 0;
	bool matrices_cached = false;
	// Game view-projection matrix cache (avoids 16 RDRAM reads per billboard call)
	float vp_mtx[16] = {};
	bool vp_cached = false;
};

static BillboardAllocator s_alloc;

static bool allocate_billboard_data(uint8_t* rdram, int bytes_needed, gpr& out_addr) {
	const int slots_needed = (bytes_needed + (BILLBOARD_SLOT_BYTES - 1)) / BILLBOARD_SLOT_BYTES;
	if ((s_alloc.used_slots + slots_needed) > BILLBOARD_POOL_SLOTS) {
		return false;
	}

	const uint32_t offset = static_cast<uint32_t>(s_alloc.used_slots) * BILLBOARD_SLOT_BYTES;
	out_addr = static_cast<gpr>(static_cast<int32_t>(BILLBOARD_POOL_VRAM + offset));
	s_alloc.used_slots += slots_needed;
	(void)rdram;
	return true;
}

// ── Per-function diagnostic stats (logged every ~5 seconds) ──────────────

struct BillboardStats {
	const char* label;
	uint64_t interval_calls = 0;
	uint64_t interval_emits = 0;
	uint64_t interval_suppresses = 0;
	uint64_t interval_skips = 0;
	uint64_t interval_fail_counts[12] = {};
	uint64_t last_log_frame = 0;
	int32_t sample_wx = 0, sample_wy = 0, sample_wz = 0;
	int32_t sample_scale = 0;
	float sample_cam_z = 0.0f;
	uint32_t sample_group_id = 0;
	bool has_sample = false;
};

static BillboardStats s_stats_6c5e44{"6C5E44(stars)"};
static BillboardStats s_stats_73f17c{"73F17C(energy-items)"};
static BillboardStats s_stats_73f800{"73F800(flowers)"};
static BillboardStats s_stats_740094{"740094(collectibles)"};
static BillboardStats s_stats_740820{"740820(trees)"};
static BillboardStats s_stats_6fa3a4{"6FA3A4(fov-masks)"};

constexpr uint64_t STATS_LOG_INTERVAL = 150;

static void maybe_log_stats(BillboardStats& s) {
	if ((g_billboard_frame_count - s.last_log_frame) < STATS_LOG_INTERVAL) return;
	s.last_log_frame = g_billboard_frame_count;
	if (s.interval_calls == 0 && s.interval_skips == 0) return;

	// Only print when the "Billboard Debug Logs" toggle is enabled.
	// Counters are always reset so toggling on mid-session shows clean data.
	if (g_log_73f17c_ortho) {
		uint64_t total_fails = 0;
		for (int i = 1; i < 12; i++) total_fails += s.interval_fail_counts[i];

		std::printf("[BILLBOARD %s] calls=%llu emit=%llu suppress=%llu skip=%llu fail=%llu",
			s.label,
			(unsigned long long)s.interval_calls,
			(unsigned long long)s.interval_emits,
			(unsigned long long)s.interval_suppresses,
			(unsigned long long)s.interval_skips,
			(unsigned long long)total_fails);

		if (total_fails > 0) {
			std::printf(" (");
			bool first = true;
			for (int i = 1; i < 12; i++) {
				if (s.interval_fail_counts[i] > 0) {
					if (!first) std::printf(",");
					std::printf("%s=%llu",
						rewrite_outcome_name(static_cast<RewriteOutcome>(i)),
						(unsigned long long)s.interval_fail_counts[i]);
					first = false;
				}
			}
			std::printf(")");
		}
		if (s.has_sample) {
			std::printf(" [sample: xyz=(%d,%d,%d) s=%d z=%.2f grp=%08X]",
				s.sample_wx, s.sample_wy, s.sample_wz,
				s.sample_scale, s.sample_cam_z, s.sample_group_id);
		}
		std::printf(" pool=%d/%d\n", s_alloc.used_slots, BILLBOARD_POOL_SLOTS);
		std::fflush(stdout);
	}

	s.interval_calls = 0;
	s.interval_emits = 0;
	s.interval_suppresses = 0;
	s.interval_skips = 0;
	std::memset(s.interval_fail_counts, 0, sizeof(s.interval_fail_counts));
	s.has_sample = false;
}

static void record_stat(BillboardStats& s, RewriteOutcome outcome, bool suppressed, const RewriteTrace* trace) {
	s.interval_calls++;
	if (outcome == RewriteOutcome::Emitted) {
		s.interval_emits++;
		if (suppressed) s.interval_suppresses++;
		if (trace && !s.has_sample) {
			s.sample_wx = trace->world_x;
			s.sample_wy = trace->world_y;
			s.sample_wz = trace->world_z;
			s.sample_scale = trace->scale;
			s.sample_cam_z = trace->cam_z;
			s.sample_group_id = trace->group_id;
			s.has_sample = true;
		}
	} else {
		const int idx = static_cast<int>(outcome);
		if (idx >= 0 && idx < 12) s.interval_fail_counts[idx]++;
	}
	maybe_log_stats(s);
}

static void record_stat_skip(BillboardStats& s) {
	s.interval_skips++;
	maybe_log_stats(s);
}

// ── End diagnostic stats ────────────────────────────────────────────────

static void write_identity(float* matrix) {
	for (int i = 0; i < 16; i++) {
		matrix[i] = 0.0f;
	}
	matrix[0] = 1.0f;
	matrix[5] = 1.0f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
}

static void write_ortho(float* matrix, float left, float right, float bottom, float top, float near_plane, float far_plane) {
	write_identity(matrix);

	const float inv_rl = 1.0f / (right - left);
	const float inv_tb = 1.0f / (top - bottom);
	const float inv_fn = 1.0f / (far_plane - near_plane);

	matrix[0] = 2.0f * inv_rl;
	matrix[5] = 2.0f * inv_tb;
	matrix[10] = -2.0f * inv_fn;
	matrix[12] = -(right + left) * inv_rl;
	matrix[13] = -(top + bottom) * inv_tb;
	matrix[14] = -(far_plane + near_plane) * inv_fn;
}

// Configuration for the generic billboard ortho-quad rewrite.
// Each billboard function passes its own config to customize scaling, geometry, etc.
struct BillboardConfig {
	uint32_t hash_salt        = 0x73F17C00u;
	float scale_clamp_min     = 0.0f;
	float scale_clamp_max     = 16383.0f;
	bool dual_scale           = false;   // if true, read independent Y scale from stack +0x1C
	int16_t geom_half_h       = 0;       // 0 = use raw half_h; nonzero = use this for geometry (TC still uses raw)
	float y_top_mul           = 1.0f;    // multiplier for yl offset (3.0 for 73F800 plants)
	float y_bottom_fixed      = 0.0f;    // > 0: use center_y + this for yh instead of center_y + y_offset (for stars)
	bool screen_wrap          = false;   // wrap center_x to [0, screen_w*4] (740820 tree tops)
	int16_t offset_clamp      = 0;       // > 0: clamp x/y offsets to this * 2 (740820)
	bool hash_includes_scale  = true;    // if false, exclude scale from group_id (for animated-scale items like Power Orbs)
	int hash_coord_shift      = 0;       // right-shift world coords before hashing/signature (quantizes pulsating positions)
};

static RewriteOutcome rewrite_billboard_ortho_quad(uint8_t* rdram, recomp_context* ctx, const BillboardConfig& cfg, RewriteTrace* out_trace) {
	// One-time tuning to avoid unordered_map rehashing in hot paths.
	static bool s_prev_quads_inited = false;
	if (!s_prev_quads_inited) {
		g_prev_quads.reserve(4096);
		s_prev_quads_inited = true;
	}

	const int32_t world_x = static_cast<int32_t>(ctx->r5);
	const int32_t world_y = static_cast<int32_t>(ctx->r6);
	const int32_t world_z = static_cast<int32_t>(ctx->r7);

	const int16_t half_w = static_cast<int16_t>(MEM_W(0x10, ctx->r29));
	const int16_t half_h = static_cast<int16_t>(MEM_W(0x14, ctx->r29));
	const int32_t scale  = static_cast<int32_t>(MEM_W(0x18, ctx->r29));
	const int32_t scale_y_raw = cfg.dual_scale ? static_cast<int32_t>(MEM_W(0x1C, ctx->r29)) : scale;

	RewriteTrace trace;
	trace.world_x = world_x;
	trace.world_y = world_y;
	trace.world_z = world_z;
	trace.half_w  = half_w;
	trace.half_h  = half_h;
	trace.scale   = scale;

	if ((half_w <= 0) || (half_h <= 0) || (scale <= 0) || (cfg.dual_scale && (scale_y_raw <= 0))) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::InvalidArgs;
	}

	const int16_t screen_w = static_cast<int16_t>(MEM_H(0, ADDR_SCREEN_WIDTH));
	const int16_t screen_h = static_cast<int16_t>(MEM_H(0, ADDR_SCREEN_HEIGHT));
	trace.screen_w = screen_w;
	trace.screen_h = screen_h;

	if ((screen_w <= 0) || (screen_h <= 0)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::InvalidScreen;
	}

	const gpr dl_state = MEM_W(0, ADDR_D_80204278_PTR);
	if (dl_state == 0) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::MissingDlState;
	}

	// Reset our extended RDRAM allocator on new frame (dl_state change).
	if (s_alloc.dl_state != dl_state) {
		s_alloc.dl_state = dl_state;
		s_alloc.used_slots = 0;
		s_alloc.matrices_cached = false;
		s_alloc.vp_cached = false;
		g_billboard_frame_count++;
	}

	// Check if we can reuse cached matrices from an earlier billboard this frame.
	const bool cache_hit = s_alloc.matrices_cached
		&& (s_alloc.screen_w == screen_w)
		&& (s_alloc.screen_h == screen_h);

	// Early capacity check: bail out before expensive math if our pool is full.
	{
		constexpr int kMtxBytes = static_cast<int>(sizeof(float) * 16);
		constexpr int kVtxBytes = static_cast<int>(sizeof(Rt64VertexExV1) * 4);
		const int kNeededBytes = cache_hit ? kVtxBytes : (2 * kMtxBytes) + kVtxBytes;
		const int kNeededSlots = (kNeededBytes + (BILLBOARD_SLOT_BYTES - 1))
		                       / BILLBOARD_SLOT_BYTES;
		if ((s_alloc.used_slots + kNeededSlots) > BILLBOARD_POOL_SLOTS) {
			if (out_trace) *out_trace = trace;
			return RewriteOutcome::AllocFail;
		}
	}

	// Read the game's view-projection matrix once per frame, then serve from cache.
	// Saves 16 RDRAM reads per billboard call after the first one each frame.
	if (!s_alloc.vp_cached) {
		const gpr m_base = ADD32(dl_state, DISPLAYLIST_OFF_VIEWPROJ_F32);
		for (int i = 0; i < 16; i++) {
			s_alloc.vp_mtx[i] = read_f32(rdram, ADD32(m_base, i * 4));
		}
		s_alloc.vp_cached = true;
	}
	const float* vp = s_alloc.vp_mtx;
	auto m = [vp](int r, int c) -> float {
		return vp[r * 4 + c];
	};

	// World coords are 16.16 fixed-point.
	const float x = static_cast<float>(world_x) / 65536.0f;
	const float y = static_cast<float>(world_y) / 65536.0f;
	const float z = static_cast<float>(world_z) / 65536.0f;

	const float cam_z = m(2, 3) + (m(2, 2) * z) + (m(2, 1) * y) + (m(2, 0) * x);
	trace.cam_z = cam_z;

	// Conservative reject: keep behind-camera threshold as a named constant.
	if (!(cam_z <= kBehindCameraZ)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::BehindCamera;
	}

	// Depth proxy used to derive prim-depth (keeps ordering close to original texrect path).
	const float clip_w = ((m(3, 2) * cam_z) + m(3, 3)) / -cam_z;
	trace.clip_w = clip_w;
	if (!(clip_w > 0.0f)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::InvalidClipW;
	}

	const float proj_x = m(0, 3) + (m(0, 2) * z) + (m(0, 1) * y) + (m(0, 0) * x);
	const float proj_y = m(1, 3) + (m(1, 2) * z) + (m(1, 1) * y) + (m(1, 0) * x);

	// Screen coordinates are in a 4x scaled space (consistent with original path).
	const float center_x = ((m(3, 0) * proj_x) / cam_z) + (static_cast<float>(screen_w) * 2.0f);
	const float center_y = ((m(3, 1) * proj_y) / cam_z) + (static_cast<float>(screen_h) * 2.0f);

	const float fov_y = read_f32(rdram, ADD32(ADDR_LEVEL_CONFIG, LEVELCFG_OFF_FOV_Y));
	if (!std::isfinite(fov_y) || (std::fabs(fov_y) < 0.0001f)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::InvalidFov;
	}

	// Sprite scaling: mirrors original behavior. Supports independent X/Y scales.
	const float scaled_x = (static_cast<float>(scale) * 33.0f) / fov_y;
	const float scaled_y = (static_cast<float>(scale_y_raw) * 33.0f) / fov_y;
	float sprite_scale_x = std::clamp((scaled_x * 32.0f) / -cam_z, cfg.scale_clamp_min, cfg.scale_clamp_max);
	float sprite_scale_y = std::clamp((scaled_y * 32.0f) / -cam_z, cfg.scale_clamp_min, cfg.scale_clamp_max);
	trace.sprite_scale = sprite_scale_x;
	if (!(sprite_scale_x > 0.0f) || !(sprite_scale_y > 0.0f)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::InvalidSpriteScale;
	}

	// Geometry may use a modified half_h (e.g. 73F800 subtracts 32 for tall plants).
	const int16_t geom_hh = (cfg.geom_half_h != 0) ? cfg.geom_half_h : half_h;

	float x_offset = (static_cast<float>(half_w) * sprite_scale_x) / 128.0f;
	float y_offset = (static_cast<float>(geom_hh) * sprite_scale_y) / 128.0f;

	// Offset clamping (740820: clamp to arg9 * 2).
	if (cfg.offset_clamp > 0) {
		const float clamp_val = static_cast<float>(cfg.offset_clamp) * 2.0f;
		x_offset = std::min(x_offset, clamp_val);
		y_offset = std::min(y_offset, clamp_val);
	}

	// Screen wrapping (740820: wrap center_x into [0, screen_w*4]).
	float adj_center_x = center_x;
	if (cfg.screen_wrap) {
		const float sw4 = static_cast<float>(screen_w) * 4.0f;
		while (adj_center_x >= sw4) adj_center_x -= sw4;
		while (adj_center_x < 0.0f) adj_center_x += sw4;
	}

	const float xl = adj_center_x - x_offset;
	const float yl = center_y - (y_offset * cfg.y_top_mul);
	const float xh = adj_center_x + x_offset;
	const float yh = (cfg.y_bottom_fixed > 0.0f) ? (center_y + cfg.y_bottom_fixed) : (center_y + y_offset);
	trace.xl = xl;
	trace.yl = yl;
	trace.xh = xh;
	trace.yh = yh;

	const float screen_max_x = static_cast<float>(screen_w) * 4.0f;
	const float screen_max_y = static_cast<float>(screen_h) * 4.0f;

	if (!((xl < xh) && (yl < yh) && (xl < screen_max_x) && (yl < screen_max_y) && (xh > 0.0f) && (yh > 0.0f))) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::Offscreen;
	}

	// Allocate from our extended RDRAM pool (NOT the game's limited vertex pool).
	// Cache hit: reuse matrices from first billboard, only allocate 4 vertices (6 slots).
	// Cache miss: allocate 2 matrices + 4 vertices (14 slots), then populate cache.
	constexpr int kMatrixBytes = static_cast<int>(sizeof(float) * 16);
	constexpr int kVertexBytes = static_cast<int>(sizeof(Rt64VertexExV1) * 4);

	gpr proj_mtx_addr, view_mtx_addr, verts_addr;

	if (cache_hit) {
		proj_mtx_addr = s_alloc.proj_mtx_addr;
		view_mtx_addr = s_alloc.view_mtx_addr;
		gpr alloc_addr = 0;
		if (!allocate_billboard_data(rdram, kVertexBytes, alloc_addr)) {
			if (out_trace) *out_trace = trace;
			return RewriteOutcome::AllocFail;
		}
		verts_addr = alloc_addr;
	} else {
		constexpr int kAllocBytes = (kMatrixBytes * 2) + kVertexBytes;
		gpr alloc_addr = 0;
		if (!allocate_billboard_data(rdram, kAllocBytes, alloc_addr)) {
			if (out_trace) *out_trace = trace;
			return RewriteOutcome::AllocFail;
		}
		proj_mtx_addr = alloc_addr;
		view_mtx_addr = ADD32(proj_mtx_addr, kMatrixBytes);
		verts_addr    = ADD32(view_mtx_addr, kMatrixBytes);

		float* proj_mtx = reinterpret_cast<float*>(rdram + vram_to_phys_u32(proj_mtx_addr));
		float* view_mtx = reinterpret_cast<float*>(rdram + vram_to_phys_u32(view_mtx_addr));

		write_ortho(
			proj_mtx,
			-static_cast<float>(screen_w) * 2.0f,
			 static_cast<float>(screen_w) * 2.0f,
			 static_cast<float>(screen_h) * 2.0f,
			-static_cast<float>(screen_h) * 2.0f,
			-1.0f,
			 1.0f
		);
		write_identity(view_mtx);

		// Populate cache for subsequent billboards this frame.
		s_alloc.proj_mtx_addr = proj_mtx_addr;
		s_alloc.view_mtx_addr = view_mtx_addr;
		s_alloc.screen_w = screen_w;
		s_alloc.screen_h = screen_h;
		s_alloc.matrices_cached = true;
	}

	// Convert to centered coordinates (ortho matrix origin at screen center).
	const float centered_xl = xl - (static_cast<float>(screen_w) * 2.0f);
	const float centered_yl = yl - (static_cast<float>(screen_h) * 2.0f);
	const float centered_xh = xh - (static_cast<float>(screen_w) * 2.0f);
	const float centered_yh = yh - (static_cast<float>(screen_h) * 2.0f);

	const int16_t cur_x[4] = {
		clamp_i16(centered_xl),
		clamp_i16(centered_xh),
		clamp_i16(centered_xl),
		clamp_i16(centered_xh)
	};
	const int16_t cur_y[4] = {
		clamp_i16(centered_yl),
		clamp_i16(centered_yl),
		clamp_i16(centered_yh),
		clamp_i16(centered_yh)
	};

	// Quantize world coords for hashing/signature when items pulsate (Power Cells).
	// Right-shifting by hash_coord_shift rounds positions to a coarser grid so small
	// frame-to-frame Z/scale oscillations don't produce a new group_id every frame.
	const int32_t q_x = world_x >> cfg.hash_coord_shift;
	const int32_t q_y = world_y >> cfg.hash_coord_shift;
	const int32_t q_z = world_z >> cfg.hash_coord_shift;

	// For items with animated scale (Power Orbs fade-out), exclude scale from hash
	// so the group_id remains stable across frames and interpolation works correctly.
	const int32_t hash_scale = cfg.hash_includes_scale ? scale : 0;
	const uint32_t group_id = billboard_group_id(q_x, q_y, q_z, half_w, half_h, hash_scale, cfg.hash_salt);
	trace.group_id = group_id;

	g_quad_stamp++;

	int16_t prev_x[4] = { cur_x[0], cur_x[1], cur_x[2], cur_x[3] };
	int16_t prev_y[4] = { cur_y[0], cur_y[1], cur_y[2], cur_y[3] };

	// Pull previous quad position for interpolation (if recent and signature matches).
	const auto prev_it = g_prev_quads.find(group_id);
	if (prev_it != g_prev_quads.end()) {
		const PrevQuad& pq = prev_it->second;
		const bool recent = ((g_quad_stamp - pq.stamp) <= 300);
		const bool sig_ok = (pq.sig_x == q_x) && (pq.sig_y == q_y) && (pq.sig_z == q_z);
		if (recent && sig_ok) {
			for (int i = 0; i < 4; i++) {
				prev_x[i] = pq.x[i];
				prev_y[i] = pq.y[i];
			}
		}
	}

	// Update cache.
	PrevQuad& prev_entry = g_prev_quads[group_id];
	for (int i = 0; i < 4; i++) {
		prev_entry.x[i] = cur_x[i];
		prev_entry.y[i] = cur_y[i];
	}
	prev_entry.sig_x = q_x;
	prev_entry.sig_y = q_y;
	prev_entry.sig_z = q_z;
	prev_entry.stamp = g_quad_stamp;

	// No periodic cleanup: the map is naturally bounded because active billboards
	// reuse the same group_ids each frame. Stale entries from billboards that left
	// view waste a small amount of memory (~50 bytes/entry) but the signature check
	// prevents false matches. Removing the cleanup avoids periodic iteration +
	// erase-during-iteration costs that caused micro-stutters every ~512 draws.

	const int16_t z_screen = 0;

	const int s_max_i = (std::max(0, static_cast<int>(half_w) - 1) << 6);
	const int t_max_i = (std::max(0, static_cast<int>(half_h) - 1) << 6);
	const int16_t s_max = static_cast<int16_t>(std::clamp(s_max_i, -32768, 32767));
	const int16_t t_max = static_cast<int16_t>(std::clamp(t_max_i, -32768, 32767));

	auto* verts = reinterpret_cast<Rt64VertexExV1*>(rdram + vram_to_phys_u32(verts_addr));
	auto set_vert = [&](int index, int16_t x0, int16_t y0, int16_t px, int16_t py, int16_t s0, int16_t t0) {
		verts[index].v.y = y0;
		verts[index].v.x = x0;
		verts[index].v.flag = 0;
		verts[index].v.z = z_screen;
		verts[index].v.t = t0;
		verts[index].v.s = s0;
		verts[index].v.a = 0xFF;
		verts[index].v.b = 0xFF;
		verts[index].v.g = 0xFF;
		verts[index].v.r = 0xFF;
		verts[index].yp = py;
		verts[index].xp = px;
		verts[index].pad = 0;
		verts[index].zp = z_screen;
	};

	set_vert(0, cur_x[0], cur_y[0], prev_x[0], prev_y[0], 0,     0);
	set_vert(1, cur_x[1], cur_y[1], prev_x[1], prev_y[1], s_max, 0);
	set_vert(2, cur_x[2], cur_y[2], prev_x[2], prev_y[2], 0,     t_max);
	set_vert(3, cur_x[3], cur_y[3], prev_x[3], prev_y[3], s_max, t_max);

	const int16_t prim_depth_bias = static_cast<int16_t>(MEM_H(LEVELCFG_OFF_PRIMDEPTH_BIAS, ADDR_LEVEL_CONFIG));
	const int32_t depth_raw = static_cast<int32_t>(std::lround((clip_w * 1023.0f * 32.0f) + 32736.0f)) - prim_depth_bias;
	const uint16_t prim_depth = static_cast<uint16_t>(depth_raw & 0xFFFF);

	// Grab current write pointer.
	GfxWriteContext wctx;
	if (!try_get_gfx_ptr(rdram, ctx->r4, wctx)) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::GfxPtrFail;
	}

	// Capacity check: ensure we won't overrun RDRAM when writing commands.
	// This is a conservative safety check (we don't know the real DL buffer size,
	// but we *do* know the max addressable RDRAM).
	//
	// Count commands we emit below:
	//   1: setprimdepth
	//   2: gEXEnable
	//   2: gEXSetRDRAMExtended
	//   2: gEXPushOtherMode
	//   1: setothermode_h
	//   2: gEXPushProjectionMatrix
	//   2: gEXMatrixFloat (proj)
	//   2: gEXMatrixFloat (modelview push+load)
	//   2: gEXSetProjMatrixFloat
	//   2: gEXSetViewMatrixFloat
	//   2: gEXMatrixGroup
	//   2: gEXVertex
	//   2: TRI2 (two triangles)
	//   1: popmtx
	//   2: gEXPopMatrixGroup
	//   2: gEXPopProjectionMatrix
	//   1: gEXSetProjMatrixFloat (restore identity)
	//   1: gEXSetViewMatrixFloat (restore identity)
	//   1: gEXSetRDRAMExtended(0)
	//   1: gEXPopOtherMode
	// Total = 34 GfxCommand words (each GfxCommand is 8 bytes).
	constexpr uint32_t kEmittedCmds = 34;
	constexpr uint32_t kEmittedBytes = kEmittedCmds * sizeof(GfxCommand);
	if (wctx.capacity_bytes < kEmittedBytes) {
		if (out_trace) *out_trace = trace;
		return RewriteOutcome::GfxCapacityFail;
	}

	GfxCommand* cmd = wctx.cmd;

	cmd->values.word0 = CMD_SETPRIMDEPTH;
	cmd->values.word1 = static_cast<uint32_t>(prim_depth) << 16;
	cmd++;

	// Ensure RT64's extended command parser is active for this path.
	gEXEnable(cmd);
	cmd++;
	gEXSetRDRAMExtended(cmd, 1);
	cmd++;

	// Force texture perspective (G_TP_PERSP) so RT64 does not apply the 0.5 UV correction.
	// With G_TP_NONE it would only show the top-left quarter of the sprite.
	gEXPushOtherMode(cmd);
	cmd++;
	cmd->values.word0 = CMD_SETOTHERMODE_H_TP_PERSP;
	cmd->values.word1 = CMD_SETOTHERMODE_H_TP_PERSP_W1;
	cmd++;

	gEXPushProjectionMatrix(cmd);
	cmd++;

	// Load ortho into the STANDARD RSP projection matrix (viewProjMatrixStack).
	// gEXSetProjMatrixFloat only sets the extended matrix, but vertex clipping
	// uses the standard RSP stack. Without this, vertices are transformed by
	// whatever 3D perspective matrix was active.
	gEXMatrixFloat(cmd, static_cast<uint32_t>(proj_mtx_addr), GEXMTX_LOAD_PROJ);
	cmd += 2;

	// Push current modelview and load identity into standard RSP modelview stack.
	gEXMatrixFloat(cmd, static_cast<uint32_t>(view_mtx_addr), GEXMTX_PUSH_LOAD_MODELVIEW);
	cmd += 2;

	// Set extended matrices for RT64's world transform / interpolation system.
	gEXSetProjMatrixFloat(cmd, static_cast<uint32_t>(proj_mtx_addr));
	cmd++;
	gEXSetViewMatrixFloat(cmd, static_cast<uint32_t>(view_mtx_addr));
	cmd++;

	gEXMatrixGroup(
		cmd,
		group_id,
		G_EX_INTERPOLATE_SIMPLE,
		G_EX_PUSH,
		0,
		G_EX_COMPONENT_SKIP,
		G_EX_COMPONENT_SKIP,
		G_EX_COMPONENT_SKIP,
		G_EX_COMPONENT_SKIP,
		G_EX_COMPONENT_SKIP,
		G_EX_COMPONENT_INTERPOLATE,
		G_EX_COMPONENT_SKIP,
		G_EX_ORDER_LINEAR,
		G_EX_EDIT_NONE,
		G_EX_ASPECT_AUTO,
		G_EX_COMPONENT_INTERPOLATE,
		G_EX_COMPONENT_SKIP
	);
	cmd += 2;

	gEXVertex(cmd, static_cast<uint32_t>(verts_addr), 4, 0);
	cmd += 2;

	// F3DEX TRI2 encoding: 7-bit vertex indices at bits 17, 9, 1.
	cmd->values.word0 = CMD_TRI2 | (0u << 17) | (1u << 9) | (3u << 1);
	cmd->values.word1 =         (0u << 17) | (3u << 9) | (2u << 1);
	cmd++;

	// Emit opposite winding too so the quad remains visible regardless of current cull mode.
	cmd->values.word0 = CMD_TRI2 | (0u << 17) | (3u << 9) | (1u << 1);
	cmd->values.word1 =         (0u << 17) | (2u << 9) | (3u << 1);
	cmd++;

	// Pop standard RSP modelview stack.
	cmd->values.word0 = CMD_POPMTX;
	cmd->values.word1 = 0x00000000; // G_MTX_MODELVIEW
	cmd++;

	gEXPopMatrixGroup(cmd, 0);
	cmd++;
	gEXPopProjectionMatrix(cmd);
	cmd++;

	// Restore extended matrices to identity. gEXSetProjMatrixFloat / gEXSetViewMatrixFloat
	// set the RT64 extended projection/view which RT64 uses for world transforms
	// (rsp.cpp:523: worldTransforms = modelMatrix * extended.viewProjMatrix).
	// Without this restore, all subsequent 3D geometry would be rendered with our
	// ortho extended projection, causing the world to disappear.
	gEXSetProjMatrixFloat(cmd, static_cast<uint32_t>(view_mtx_addr));
	cmd++;
	gEXSetViewMatrixFloat(cmd, static_cast<uint32_t>(view_mtx_addr));
	cmd++;

	// CRITICAL: Disable extended RDRAM addressing. gEXPushOtherMode / gEXPopOtherMode
	// do NOT save/restore the extendRDRAM flag (it's separate from OtherMode H/L).
	// Leaving extendRDRAM=true corrupts how RT64 resolves all subsequent addresses
	// via fromSegmented(), maskPhysicalAddress(), and RDP::maskAddress(), which breaks
	// texture loads and vertex references for all remaining display list commands.
	gEXSetRDRAMExtended(cmd, 0);
	cmd++;

	gEXPopOtherMode(cmd);
	cmd++;

	advance_gfx_ptr(rdram, wctx, cmd, ctx->r4);

	if (out_trace) *out_trace = trace;
	return RewriteOutcome::Emitted;
}

} // namespace

namespace sssv::billboard {

void set_disable_6fa3a4_render(bool enabled) {
	g_disable_6fa3a4_render = enabled;
}

bool get_disable_6fa3a4_render() {
	return g_disable_6fa3a4_render;
}

void set_disable_6c5e44_render(bool enabled) {
	g_disable_6c5e44_render = enabled;
}

bool get_disable_6c5e44_render() {
	return g_disable_6c5e44_render;
}

void set_disable_73f17c_render(bool enabled) {
	g_disable_73f17c_render = enabled;
}

bool get_disable_73f17c_render() {
	return g_disable_73f17c_render;
}

void set_disable_73f800_render(bool enabled) {
	g_disable_73f800_render = enabled;
}

bool get_disable_73f800_render() {
	return g_disable_73f800_render;
}

void set_disable_740094_render(bool enabled) {
	g_disable_740094_render = enabled;
}

bool get_disable_740094_render() {
	return g_disable_740094_render;
}

void set_disable_740820_render(bool enabled) {
	g_disable_740820_render = enabled;
}

bool get_disable_740820_render() {
	return g_disable_740820_render;
}

void set_rewrite_6c5e44_ortho(bool v) { g_rewrite_6c5e44_ortho = v; }
bool get_rewrite_6c5e44_ortho() { return g_rewrite_6c5e44_ortho; }
void set_rewrite_6c5e44_suppress_original(bool v) { g_rewrite_6c5e44_suppress_original = v; }
bool get_rewrite_6c5e44_suppress_original() { return g_rewrite_6c5e44_suppress_original; }

void set_rewrite_73f17c_ortho(bool v) { g_rewrite_73f17c_ortho = v; }
bool get_rewrite_73f17c_ortho() { return g_rewrite_73f17c_ortho; }
void set_rewrite_73f17c_suppress_original(bool v) { g_rewrite_73f17c_suppress_original = v; }
bool get_rewrite_73f17c_suppress_original() { return g_rewrite_73f17c_suppress_original; }

void set_rewrite_73f800_ortho(bool v) { g_rewrite_73f800_ortho = v; }
bool get_rewrite_73f800_ortho() { return g_rewrite_73f800_ortho; }
void set_rewrite_73f800_suppress_original(bool v) { g_rewrite_73f800_suppress_original = v; }
bool get_rewrite_73f800_suppress_original() { return g_rewrite_73f800_suppress_original; }

void set_rewrite_740094_ortho(bool v) { g_rewrite_740094_ortho = v; }
bool get_rewrite_740094_ortho() { return g_rewrite_740094_ortho; }
void set_rewrite_740094_suppress_original(bool v) { g_rewrite_740094_suppress_original = v; }
bool get_rewrite_740094_suppress_original() { return g_rewrite_740094_suppress_original; }

void set_rewrite_740820_ortho(bool v) { g_rewrite_740820_ortho = v; }
bool get_rewrite_740820_ortho() { return g_rewrite_740820_ortho; }
void set_rewrite_740820_suppress_original(bool v) { g_rewrite_740820_suppress_original = v; }
bool get_rewrite_740820_suppress_original() { return g_rewrite_740820_suppress_original; }

void set_log_73f17c_ortho(bool v) { g_log_73f17c_ortho = v; }
bool get_log_73f17c_ortho() { return g_log_73f17c_ortho; }

} // namespace sssv::billboard

extern "C" void sssv_log_billboard_draw_6fa3a4(uint8_t* rdram, recomp_context* ctx) {
	s_stats_6fa3a4.interval_calls++;
	maybe_log_stats(s_stats_6fa3a4);

	if (!g_disable_6fa3a4_render) {
		return;
	}

	s_stats_6fa3a4.interval_suppresses++;
	MEM_W(0, ctx->r29 + 0x10) = 100;
	(void)rdram;
}

extern "C" void sssv_hook_billboard_6c5e44(uint8_t* rdram, recomp_context* ctx) {
	if (g_disable_6c5e44_render) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		return;
	}
	if (!g_rewrite_6c5e44_ortho) {
		record_stat_skip(s_stats_6c5e44);
		return;
	}
	BillboardConfig cfg;
	cfg.hash_salt = 0x6C5E4400u;
	cfg.scale_clamp_min = 4.0f;
	cfg.scale_clamp_max = 15.0f;
	cfg.y_bottom_fixed = 2.0f;
	RewriteTrace trace;
	const RewriteOutcome outcome = rewrite_billboard_ortho_quad(rdram, ctx, cfg, &trace);
	const bool suppressed = (outcome == RewriteOutcome::Emitted) && g_rewrite_6c5e44_suppress_original;
	record_stat(s_stats_6c5e44, outcome, suppressed, &trace);
	if (suppressed) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
	}
}

extern "C" void sssv_hook_billboard_73f800(uint8_t* rdram, recomp_context* ctx) {
	if (g_disable_73f800_render) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		return;
	}
	if (!g_rewrite_73f800_ortho) {
		record_stat_skip(s_stats_73f800);
		return;
	}
	BillboardConfig cfg;
	cfg.hash_salt = 0x73F80000u;
	cfg.hash_includes_scale = false;   // Power Cells pulsate scale every frame
	cfg.hash_coord_shift = 18;         // Quantize coords: 2^18 covers ~4 world-unit Z pulsation
	const int16_t raw_half_h = static_cast<int16_t>(MEM_W(0x14, ctx->r29));
	if (raw_half_h > 32) {
		cfg.geom_half_h = raw_half_h - 32;
		cfg.y_top_mul = 3.0f;
	}
	RewriteTrace trace;
	const RewriteOutcome outcome = rewrite_billboard_ortho_quad(rdram, ctx, cfg, &trace);
	const bool suppressed = (outcome == RewriteOutcome::Emitted) && g_rewrite_73f800_suppress_original;
	record_stat(s_stats_73f800, outcome, suppressed, &trace);
	if (suppressed) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
	}
}

extern "C" void sssv_hook_billboard_740094(uint8_t* rdram, recomp_context* ctx) {
	if (g_disable_740094_render) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		MEM_W(0, ctx->r29 + 0x1C) = 0;
		return;
	}
	if (!g_rewrite_740094_ortho) {
		record_stat_skip(s_stats_740094);
		return;
	}
	BillboardConfig cfg;
	cfg.hash_salt = 0x74009400u;
	cfg.dual_scale = true;
	cfg.hash_includes_scale = false;
	RewriteTrace trace;
	const RewriteOutcome outcome = rewrite_billboard_ortho_quad(rdram, ctx, cfg, &trace);
	const bool suppressed = (outcome == RewriteOutcome::Emitted) && g_rewrite_740094_suppress_original;
	record_stat(s_stats_740094, outcome, suppressed, &trace);
	if (suppressed) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		MEM_W(0, ctx->r29 + 0x1C) = 0;
	}
}

extern "C" void sssv_hook_billboard_740820(uint8_t* rdram, recomp_context* ctx) {
	if (g_disable_740820_render) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		MEM_W(0, ctx->r29 + 0x1C) = 0;
		return;
	}
	if (!g_rewrite_740820_ortho) {
		record_stat_skip(s_stats_740820);
		return;
	}
	BillboardConfig cfg;
	cfg.hash_salt = 0x74082000u;
	cfg.dual_scale = true;
	cfg.screen_wrap = (static_cast<uint8_t>(MEM_W(0x20, ctx->r29)) != 0);
	cfg.offset_clamp = static_cast<int16_t>(MEM_W(0x24, ctx->r29));
	RewriteTrace trace;
	const RewriteOutcome outcome = rewrite_billboard_ortho_quad(rdram, ctx, cfg, &trace);
	const bool suppressed = (outcome == RewriteOutcome::Emitted) && g_rewrite_740820_suppress_original;
	record_stat(s_stats_740820, outcome, suppressed, &trace);
	if (suppressed) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		MEM_W(0, ctx->r29 + 0x1C) = 0;
	}
}

extern "C" void sssv_log_billboard_draw_73f17c(uint8_t* rdram, recomp_context* ctx) {
	if (g_disable_73f17c_render) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
		return;
	}

	if (!g_rewrite_73f17c_ortho) {
		record_stat_skip(s_stats_73f17c);
		return;
	}

	BillboardConfig cfg_73f17c;
	RewriteTrace trace;
	const RewriteOutcome outcome = rewrite_billboard_ortho_quad(rdram, ctx, cfg_73f17c, &trace);
	const bool suppressed = (outcome == RewriteOutcome::Emitted) && g_rewrite_73f17c_suppress_original;
	record_stat(s_stats_73f17c, outcome, suppressed, &trace);

#if SSSV_BILLBOARD_DEBUG
	if (g_log_73f17c_ortho) {
		static uint64_t s_call_count = 0;
		s_call_count++;
		const bool emit_log = (outcome != RewriteOutcome::Emitted) || ((s_call_count & 0x3Fu) == 0);
		if (emit_log) {
			std::printf(
				"[73F17C-ORTHO] n=%llu %s scale=%d hw=%d hh=%d scr=%dx%d z=%.3f cw=%.3f s=%.3f rect=(%.1f,%.1f)-(%.1f,%.1f) grp=%08X suppress=%d\n",
				static_cast<unsigned long long>(s_call_count),
				rewrite_outcome_name(outcome),
				trace.scale,
				static_cast<int>(trace.half_w),
				static_cast<int>(trace.half_h),
				static_cast<int>(trace.screen_w),
				static_cast<int>(trace.screen_h),
				trace.cam_z,
				trace.clip_w,
				trace.sprite_scale,
				trace.xl,
				trace.yl,
				trace.xh,
				trace.yh,
				trace.group_id,
				g_rewrite_73f17c_suppress_original ? 1 : 0
			);
			if (outcome != RewriteOutcome::Emitted) {
				std::fflush(stdout);
			}
		}
	}
#endif

	if (suppressed) {
		MEM_W(0, ctx->r29 + 0x18) = 0;
	}
}
