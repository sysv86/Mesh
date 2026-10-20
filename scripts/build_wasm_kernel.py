import struct, sys

# Helper functions for encoding standard WebAssembly 1.0 binary format (LEB128 + sections)
def uleb(val):
    out = bytearray()
    while True:
        b = val & 0x7F
        val >>= 7
        if val == 0:
            out.append(b)
            break
        out.append(b | 0x80)
    return bytes(out)

def sleb(val):
    out = bytearray()
    while True:
        b = val & 0x7F
        val >>= 7
        if (val == 0 and (b & 0x40) == 0) or (val == -1 and (b & 0x40) != 0):
            out.append(b)
            break
        out.append(b | 0x80)
    return bytes(out)

def f32_const(v):
    return b'\x43' + struct.pack('<f', float(v))

def i32_const(v):
    v = int(v) & 0xFFFFFFFF
    if v >= 0x80000000: v -= 0x100000000
    return b'\x41' + sleb(int(v))

def section(sec_id, payload):
    return bytes([sec_id]) + uleb(len(payload)) + payload

def vec(items):
    return uleb(len(items)) + b''.join(items)

def encode_str(s):
    b = s.encode('utf-8')
    return uleb(len(b)) + b

# Wasm types
I32 = b'\x7F'
F32 = b'\x7D'

def func_type(params, results):
    return b'\x60' + vec(params) + vec(results)

# Opcodes
LOCAL_GET = lambda idx: b'\x20' + uleb(idx)
LOCAL_SET = lambda idx: b'\x21' + uleb(idx)
LOCAL_TEE = lambda idx: b'\x22' + uleb(idx)

I32_LOAD  = lambda align=2, offset=0: b'\x28' + uleb(align) + uleb(offset)
I32_STORE = lambda align=2, offset=0: b'\x36' + uleb(align) + uleb(offset)

I32_EQZ = b'\x45'
I32_EQ  = b'\x46'
I32_LT_S = b'\x48'
I32_GT_S = b'\x4A'
I32_LE_S = b'\x4C'
I32_GE_S = b'\x4E'
I32_ADD = b'\x6A'
I32_SUB = b'\x6B'
I32_MUL = b'\x6C'
I32_AND = b'\x71'
I32_OR  = b'\x72'
I32_SHL = b'\x74'
I32_SHR_U = b'\x76'

F32_LT = b'\x5D'
F32_GT = b'\x5E'
F32_LE = b'\x5F'
F32_GE = b'\x60'
F32_ABS = b'\x8B'
F32_NEG = b'\x8C'
F32_SQRT = b'\x91'
F32_ADD = b'\x92'
F32_SUB = b'\x93'
F32_MUL = b'\x94'
F32_DIV = b'\x95'
F32_MIN = b'\x96'
F32_MAX = b'\x97'

F32_CONVERT_I32_S = b'\xB2'
I32_TRUNC_F32_S   = b'\xA8'

BLOCK_VOID = b'\x02\x40'
LOOP_VOID  = b'\x03\x40'
IF_VOID    = b'\x04\x40'
IF_F32     = b'\x04\x7D'
ELSE       = b'\x05'
END        = b'\x0B'
BR         = lambda d: b'\x0C' + uleb(d)
BR_IF      = lambda d: b'\x0D' + uleb(d)
RETURN     = b'\x0F'
CALL       = lambda f: b'\x10' + uleb(f)

# Function 0: wasm_version() -> i32 (returns 10000 for v1.0.0)
f0_type = func_type([], [I32])
f0_body = vec([]) + i32_const(10000) + END

# Function 1: wasm_sdf_rounded_box(px, py, cx, cy, hx, hy, r) -> f32
# params: 0:px, 1:py, 2:cx, 3:cy, 4:hx, 5:hy, 6:r
# locals: 7:qx(f32), 8:qy(f32), 9:qx_pos(f32), 10:qy_pos(f32)
f1_type = func_type([F32]*7, [F32])
f1_code = (
    # qx = abs(px - cx) - hx + r
    LOCAL_GET(0) + LOCAL_GET(2) + F32_SUB + F32_ABS + LOCAL_GET(4) + F32_SUB + LOCAL_GET(6) + F32_ADD + LOCAL_SET(7) +
    # qy = abs(py - cy) - hy + r
    LOCAL_GET(1) + LOCAL_GET(3) + F32_SUB + F32_ABS + LOCAL_GET(5) + F32_SUB + LOCAL_GET(6) + F32_ADD + LOCAL_SET(8) +
    # qx_pos = max(qx, 0.0)
    LOCAL_GET(7) + f32_const(0.0) + F32_MAX + LOCAL_SET(9) +
    # qy_pos = max(qy, 0.0)
    LOCAL_GET(8) + f32_const(0.0) + F32_MAX + LOCAL_SET(10) +
    # min(max(qx, qy), 0.0) + sqrt(qx_pos*qx_pos + qy_pos*qy_pos) - r
    LOCAL_GET(7) + LOCAL_GET(8) + F32_MAX + f32_const(0.0) + F32_MIN +
    LOCAL_GET(9) + LOCAL_GET(9) + F32_MUL + LOCAL_GET(10) + LOCAL_GET(10) + F32_MUL + F32_ADD + F32_SQRT +
    F32_ADD + LOCAL_GET(6) + F32_SUB + END
)
f1_body = vec([uleb(4) + F32]) + f1_code

# Function 2: wasm_smin(a: f32, b: f32, k: f32) -> f32
# params: 0:a, 1:b, 2:k; locals: 3:h(f32)
f2_type = func_type([F32, F32, F32], [F32])
f2_code = (
    # h = min(max(0.5 + 0.5 * (b - a) / max(k, 0.001), 0.0), 1.0)
    f32_const(0.5) + f32_const(0.5) + LOCAL_GET(1) + LOCAL_GET(0) + F32_SUB + F32_MUL +
    LOCAL_GET(2) + f32_const(0.001) + F32_MAX + F32_DIV + F32_ADD +
    f32_const(0.0) + F32_MAX + f32_const(1.0) + F32_MIN + LOCAL_SET(3) +
    # return (b + (a - b) * h) - k * h * (1.0 - h)
    LOCAL_GET(1) + LOCAL_GET(0) + LOCAL_GET(1) + F32_SUB + LOCAL_GET(3) + F32_MUL + F32_ADD +
    LOCAL_GET(2) + LOCAL_GET(3) + F32_MUL + f32_const(1.0) + LOCAL_GET(3) + F32_SUB + F32_MUL + F32_SUB + END
)
f2_body = vec([uleb(1) + F32]) + f2_code

# Function 3: wasm_sdf_metaball3(px, py, x1, y1, r1, x2, y2, r2, x3, y3, r3, k) -> f32
f3_type = func_type([F32]*12, [F32])
f3_code = (
    # d1 = sqrt((px-x1)^2 + (py-y1)^2) - r1
    LOCAL_GET(0) + LOCAL_GET(2) + F32_SUB + LOCAL_GET(0) + LOCAL_GET(2) + F32_SUB + F32_MUL +
    LOCAL_GET(1) + LOCAL_GET(3) + F32_SUB + LOCAL_GET(1) + LOCAL_GET(3) + F32_SUB + F32_MUL + F32_ADD + F32_SQRT + LOCAL_GET(4) + F32_SUB +
    # d2 = sqrt((px-x2)^2 + (py-y2)^2) - r2
    LOCAL_GET(0) + LOCAL_GET(5) + F32_SUB + LOCAL_GET(0) + LOCAL_GET(5) + F32_SUB + F32_MUL +
    LOCAL_GET(1) + LOCAL_GET(6) + F32_SUB + LOCAL_GET(1) + LOCAL_GET(6) + F32_SUB + F32_MUL + F32_ADD + F32_SQRT + LOCAL_GET(7) + F32_SUB +
    LOCAL_GET(11) + CALL(2) +
    # d3 = sqrt((px-x3)^2 + (py-y3)^2) - r3
    LOCAL_GET(0) + LOCAL_GET(8) + F32_SUB + LOCAL_GET(0) + LOCAL_GET(8) + F32_SUB + F32_MUL +
    LOCAL_GET(1) + LOCAL_GET(9) + F32_SUB + LOCAL_GET(1) + LOCAL_GET(9) + F32_SUB + F32_MUL + F32_ADD + F32_SQRT + LOCAL_GET(10) + F32_SUB +
    LOCAL_GET(11) + CALL(2) + END
)
f3_body = vec([]) + f3_code

# Function 4: wasm_blend_pixel(addr: i32, r: i32, g: i32, b: i32, alpha256: i32)
# Blends (r, g, b) with coverage alpha256 [0..256] into RGBA pixel at linear memory `addr`
# params: 0:addr, 1:r, 2:g, 3:b, 4:a256
# locals: 5:dst(i32), 6:inv_a(i32), 7:dr(i32), 8:dg(i32), 9:db(i32), 10:out(i32)
f4_type = func_type([I32]*5, [])
f4_code = (
    LOCAL_GET(4) + i32_const(1) + I32_LT_S + IF_VOID + RETURN + END +
    LOCAL_GET(4) + i32_const(255) + I32_GE_S + IF_VOID +
        LOCAL_GET(0) +
        LOCAL_GET(1) +
        LOCAL_GET(2) + i32_const(8) + I32_SHL + I32_OR +
        LOCAL_GET(3) + i32_const(16) + I32_SHL + I32_OR +
        i32_const(0xFF000000) + I32_OR +
        I32_STORE() + RETURN +
    END +
    LOCAL_GET(0) + I32_LOAD() + LOCAL_SET(5) +
    i32_const(256) + LOCAL_GET(4) + I32_SUB + LOCAL_SET(6) +
    # dr = ((r * a256 + (dst & 0xFF) * inv_a) >> 8)
    LOCAL_GET(1) + LOCAL_GET(4) + I32_MUL + LOCAL_GET(5) + i32_const(255) + I32_AND + LOCAL_GET(6) + I32_MUL + I32_ADD + i32_const(8) + I32_SHR_U + LOCAL_SET(7) +
    # dg = ((g * a256 + ((dst >> 8) & 0xFF) * inv_a) >> 8)
    LOCAL_GET(2) + LOCAL_GET(4) + I32_MUL + LOCAL_GET(5) + i32_const(8) + I32_SHR_U + i32_const(255) + I32_AND + LOCAL_GET(6) + I32_MUL + I32_ADD + i32_const(8) + I32_SHR_U + LOCAL_SET(8) +
    # db = ((b * a256 + ((dst >> 16) & 0xFF) * inv_a) >> 8)
    LOCAL_GET(3) + LOCAL_GET(4) + I32_MUL + LOCAL_GET(5) + i32_const(16) + I32_SHR_U + i32_const(255) + I32_AND + LOCAL_GET(6) + I32_MUL + I32_ADD + i32_const(8) + I32_SHR_U + LOCAL_SET(9) +
    LOCAL_GET(0) +
    LOCAL_GET(7) +
    LOCAL_GET(8) + i32_const(8) + I32_SHL + I32_OR +
    LOCAL_GET(9) + i32_const(16) + I32_SHL + I32_OR +
    i32_const(0xFF000000) + I32_OR +
    I32_STORE() + END
)
f4_body = vec([uleb(6) + I32]) + f4_code

# Function 5: wasm_fill_buffer(ptr: i32, pixel_count: i32, abgr_color: i32)
# params: 0:ptr, 1:count, 2:col; locals: 3:i(i32)
f5_type = func_type([I32, I32, I32], [])
f5_code = (
    i32_const(0) + LOCAL_SET(3) +
    BLOCK_VOID + LOOP_VOID +
        LOCAL_GET(3) + LOCAL_GET(1) + I32_GE_S + BR_IF(1) +
        LOCAL_GET(0) + LOCAL_GET(3) + i32_const(4) + I32_MUL + I32_ADD + LOCAL_GET(2) + I32_STORE() +
        LOCAL_GET(3) + i32_const(1) + I32_ADD + LOCAL_SET(3) +
        BR(0) +
    END + END + END
)
f5_body = vec([uleb(1) + I32]) + f5_code

# Function 6: wasm_render_metaballs_rect(
#   ptr: i32, width: i32, x0: i32, y0: i32, x1: i32, y1: i32,
#   x1_c: f32, y1_c: f32, r1: f32,
#   x2_c: f32, y2_c: f32, r2: f32,
#   x3_c: f32, y3_c: f32, r3: f32,
#   k: f32, glow_r: f32,
#   r0: i32, g0: i32, b0: i32,
#   r1_col: i32, g1_col: i32, b1_col: i32
# )
# Renders an analytical SDF smooth_union(k) metaball cluster with gradient + glow directly in Wasm linear memory!
f6_params = [I32]*6 + [F32]*11 + [I32]*6
f6_type = func_type(f6_params, [])
# locals: 23:py(i32), 24:px(i32), 25:d(f32), 26:cov(f32), 27:t_grad(f32), 28:addr(i32), 29:cr(i32), 30:cg(i32), 31:cb(i32), 32:a256(i32)
f6_code = (
    LOCAL_GET(3) + LOCAL_SET(23) + # py = y0
    BLOCK_VOID + LOOP_VOID +
        LOCAL_GET(23) + LOCAL_GET(5) + I32_GT_S + BR_IF(1) +
        LOCAL_GET(2) + LOCAL_SET(24) + # px = x0
        BLOCK_VOID + LOOP_VOID +
            LOCAL_GET(24) + LOCAL_GET(4) + I32_GT_S + BR_IF(1) +
            # d = wasm_sdf_metaball3(px + 0.5, py + 0.5, ...)
            LOCAL_GET(24) + F32_CONVERT_I32_S + f32_const(0.5) + F32_ADD +
            LOCAL_GET(23) + F32_CONVERT_I32_S + f32_const(0.5) + F32_ADD +
            LOCAL_GET(6) + LOCAL_GET(7) + LOCAL_GET(8) +
            LOCAL_GET(9) + LOCAL_GET(10) + LOCAL_GET(11) +
            LOCAL_GET(12) + LOCAL_GET(13) + LOCAL_GET(14) +
            LOCAL_GET(15) + CALL(3) + LOCAL_SET(25) +

            # If d < glow_r:
            LOCAL_GET(25) + LOCAL_GET(16) + F32_LT + IF_VOID +
                # addr = ptr + (py * width + px) * 4
                LOCAL_GET(0) + LOCAL_GET(23) + LOCAL_GET(1) + I32_MUL + LOCAL_GET(24) + I32_ADD + i32_const(4) + I32_MUL + I32_ADD + LOCAL_SET(28) +
                # t_grad = clamp((px - x0) / max(1, x1 - x0), 0, 1)
                LOCAL_GET(24) + LOCAL_GET(2) + I32_SUB + F32_CONVERT_I32_S +
                LOCAL_GET(4) + LOCAL_GET(2) + I32_SUB + F32_CONVERT_I32_S + f32_const(1.0) + F32_MAX + F32_DIV +
                f32_const(0.0) + F32_MAX + f32_const(1.0) + F32_MIN + LOCAL_SET(27) +
                # cr = r0 + int((r1_col - r0) * t_grad)
                LOCAL_GET(17) + LOCAL_GET(20) + LOCAL_GET(17) + I32_SUB + F32_CONVERT_I32_S + LOCAL_GET(27) + F32_MUL + I32_TRUNC_F32_S + I32_ADD + LOCAL_SET(29) +
                LOCAL_GET(18) + LOCAL_GET(21) + LOCAL_GET(18) + I32_SUB + F32_CONVERT_I32_S + LOCAL_GET(27) + F32_MUL + I32_TRUNC_F32_S + I32_ADD + LOCAL_SET(30) +
                LOCAL_GET(19) + LOCAL_GET(22) + LOCAL_GET(19) + I32_SUB + F32_CONVERT_I32_S + LOCAL_GET(27) + F32_MUL + I32_TRUNC_F32_S + I32_ADD + LOCAL_SET(31) +

                # Glow when d > -1.0 and glow_r > 1.0
                LOCAL_GET(25) + f32_const(-1.0) + F32_GT + IF_VOID +
                    # cov = (1.0 - max(0.0, d) / max(1.0, glow_r))
                    f32_const(1.0) + LOCAL_GET(25) + f32_const(0.0) + F32_MAX + LOCAL_GET(16) + f32_const(1.0) + F32_MAX + F32_DIV + F32_SUB +
                    f32_const(0.0) + F32_MAX + LOCAL_SET(26) +
                    LOCAL_GET(26) + LOCAL_GET(26) + F32_MUL + f32_const(115.0) + F32_MUL + I32_TRUNC_F32_S + LOCAL_SET(32) +
                    LOCAL_GET(28) + LOCAL_GET(29) + LOCAL_GET(30) + LOCAL_GET(31) + LOCAL_GET(32) + CALL(4) +
                END +

                # Fill when d < 1.0
                LOCAL_GET(25) + f32_const(1.0) + F32_LT + IF_VOID +
                    f32_const(0.5) + LOCAL_GET(25) + F32_SUB + f32_const(0.0) + F32_MAX + f32_const(1.0) + F32_MIN +
                    f32_const(256.0) + F32_MUL + I32_TRUNC_F32_S + LOCAL_SET(32) +
                    LOCAL_GET(28) + LOCAL_GET(29) + LOCAL_GET(30) + LOCAL_GET(31) + LOCAL_GET(32) + CALL(4) +
                END +

                # Inner rim highlight when abs(d + 1.0) < 1.2
                LOCAL_GET(25) + f32_const(1.0) + F32_ADD + F32_ABS + f32_const(1.2) + F32_LT + IF_VOID +
                    f32_const(1.2) + LOCAL_GET(25) + f32_const(1.0) + F32_ADD + F32_ABS + F32_SUB + f32_const(120.0) + F32_MUL + I32_TRUNC_F32_S + LOCAL_SET(32) +
                    LOCAL_GET(28) + i32_const(255) + i32_const(255) + i32_const(255) + LOCAL_GET(32) + CALL(4) +
                END +
            END +

            LOCAL_GET(24) + i32_const(1) + I32_ADD + LOCAL_SET(24) +
            BR(0) +
        END + END +
        LOCAL_GET(23) + i32_const(1) + I32_ADD + LOCAL_SET(23) +
        BR(0) +
    END + END + END
)
f6_body = vec([uleb(2) + I32, uleb(3) + F32, uleb(5) + I32]) + f6_code

types = [f0_type, f1_type, f2_type, f3_type, f4_type, f5_type, f6_type]
funcs = [uleb(i) for i in range(len(types))]
bodies = [b''.join([uleb(len(b)), b]) for b in [f0_body, f1_body, f2_body, f3_body, f4_body, f5_body, f6_body]]

# Memory section: 1 memory with min 64 pages (4 MB), max 256 pages (16 MB)
mem_sec = section(5, vec([b'\x01' + uleb(64) + uleb(256)]))

# Exports
exports = [
    encode_str("memory") + b'\x02' + uleb(0),
    encode_str("wasm_version") + b'\x00' + uleb(0),
    encode_str("wasm_sdf_rounded_box") + b'\x00' + uleb(1),
    encode_str("wasm_smin") + b'\x00' + uleb(2),
    encode_str("wasm_sdf_metaball3") + b'\x00' + uleb(3),
    encode_str("wasm_blend_pixel") + b'\x00' + uleb(4),
    encode_str("wasm_fill_buffer") + b'\x00' + uleb(5),
    encode_str("wasm_render_metaballs_rect") + b'\x00' + uleb(6),
]

wasm_bytes = (
    b'\x00asm\x01\x00\x00\x00' +
    section(1, vec(types)) +
    section(3, vec(funcs)) +
    mem_sec +
    section(7, vec(exports)) +
    section(10, vec(bodies))
)

with open(sys.argv[1] if len(sys.argv) > 1 else "web/mesh_kernel.wasm", "wb") as f:
    f.write(wasm_bytes)

print("Generated /tmp/mesh_kernel.wasm:", len(wasm_bytes), "bytes")
