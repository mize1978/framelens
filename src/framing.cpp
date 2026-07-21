#include "framing.hpp"
#include "json.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>

namespace fl {

// ---- 物理定数（部材断面 mm・変わらない）-------------------------------------
namespace {
constexpr float M  = 455.0f;
constexpr float T  = 38.0f;
constexpr float W4 = 89.0f;
constexpr float W6 = 140.0f;
constexpr float H8 = 184.0f;

// ① 種別の表示名
const char* typeName(MemberType t) {
    switch (t) {
        case MemberType::Plate:  return "Plate";
        case MemberType::Stud:   return "Stud";
        case MemberType::Header: return "Header";
        case MemberType::Joist:  return "Joist";
        case MemberType::Rafter: return "Rafter";
        case MemberType::Ridge:  return "Ridge";
    }
    return "Unknown";
}

// ② Zone の表示名
const char* zoneName(Zone z) {
    switch (z) {
        case Zone::South: return "South";
        case Zone::North: return "North";
        case Zone::East:  return "East";
        case Zone::West:  return "West";
        case Zone::Floor: return "Floor";
        case Zone::Roof:  return "Roof";
    }
    return "?";
}

// ① Zone が壁面かどうか（" Wall" サフィックス用）
bool isWall(Zone z) {
    return z == Zone::South || z == Zone::North || z == Zone::East || z == Zone::West;
}

// ③ Material の判定（断面最短×次の辺 で素材規格を推定）
const char* materialOf(const Member& m) {
    float dims[3] = {m.half.x*2.0f, m.half.y*2.0f, m.half.z*2.0f};
    std::sort(dims, dims + 3);
    int w = static_cast<int>(dims[1] + 0.5f);
    if (w <= 90)  return "SPF 2x4";
    if (w <= 145) return "SPF 2x6";
    return "SPF 2x8";
}

// ① ID プレフィックス
const char* idPrefix(MemberType t) {
    switch (t) {
        case MemberType::Plate:  return "PL";
        case MemberType::Stud:   return "S";
        case MemberType::Header: return "H";
        case MemberType::Joist:  return "J";
        case MemberType::Rafter: return "R";
        case MemberType::Ridge:  return "RG";
    }
    return "X";
}

inline void addBox(std::vector<Member>& out, Vec3 lo, Vec3 hi, MemberType t) {
    Vec3 c{(lo.x+hi.x)*0.5f, (lo.y+hi.y)*0.5f, (lo.z+hi.z)*0.5f};
    Vec3 h{std::fabs(hi.x-lo.x)*0.5f, std::fabs(hi.y-lo.y)*0.5f, std::fabs(hi.z-lo.z)*0.5f};
    Member m; m.center = c; m.half = h; m.type = t;
    out.push_back(m);
}

bool insideOpening(float x, const std::vector<Opening>& ops, float x0) {
    for (const auto& o : ops) {
        float s = x0 + o.x0, e = s + o.w;
        if (x > s - T && x < e + T) return true;
    }
    return false;
}

// X 方向に走る壁（前後）。z はゾーン。
void buildWallAlongX(std::vector<Member>& out, float zc, float x0, float x1,
                     float wallH, const std::vector<Opening>& ops, Zone z) {
    const size_t startIdx = out.size();
    const float zlo = zc - W4*0.5f, zhi = zc + W4*0.5f;
    addBox(out, {x0, 0.0f, zlo},        {x1, T, zhi},           MemberType::Plate);
    addBox(out, {x0, wallH-2*T, zlo},   {x1, wallH-T, zhi},     MemberType::Plate);
    addBox(out, {x0, wallH-T, zlo},     {x1, wallH, zhi},        MemberType::Plate);

    for (float x = x0; x <= x1 + 0.5f; x += M) {
        float xp = std::min(x, x1);
        if (insideOpening(xp, ops, x0)) continue;
        addBox(out, {xp-T*0.5f, T, zlo}, {xp+T*0.5f, wallH-2*T, zhi}, MemberType::Stud);
    }
    for (const auto& o : ops) {
        float s = x0 + o.x0, e = s + o.w;
        addBox(out, {s-T*0.5f, T, zlo},         {s+T*0.5f, wallH-2*T, zhi}, MemberType::Stud);
        addBox(out, {e-T*0.5f, T, zlo},         {e+T*0.5f, wallH-2*T, zhi}, MemberType::Stud);
        addBox(out, {s-T*0.5f, o.head, zlo},    {e+T*0.5f, o.head+H8, zhi}, MemberType::Header);
        if (o.sill > 0.0f)
            addBox(out, {s-T*0.5f, o.sill, zlo}, {e+T*0.5f, o.sill+T, zhi}, MemberType::Plate);
        for (float x = s+M; x < e; x += M)
            addBox(out, {x-T*0.5f, o.head+H8, zlo}, {x+T*0.5f, wallH-2*T, zhi}, MemberType::Stud);
        if (o.sill > 0.0f)
            for (float x = s+M; x < e; x += M)
                addBox(out, {x-T*0.5f, T, zlo}, {x+T*0.5f, o.sill, zhi}, MemberType::Stud);
    }
    for (size_t i = startIdx; i < out.size(); ++i) out[i].zone = z;
}

// Z 方向に走る壁（左右）。
void buildWallAlongZ(std::vector<Member>& out, float xc, float z0, float z1,
                     float wallH, Zone z) {
    const size_t startIdx = out.size();
    const float xlo = xc - W4*0.5f, xhi = xc + W4*0.5f;
    addBox(out, {xlo, 0.0f, z0},       {xhi, T, z1},          MemberType::Plate);
    addBox(out, {xlo, wallH-2*T, z0},  {xhi, wallH-T, z1},   MemberType::Plate);
    addBox(out, {xlo, wallH-T, z0},    {xhi, wallH, z1},      MemberType::Plate);
    for (float zp = z0; zp <= z1 + 0.5f; zp += M) {
        float zc2 = std::min(zp, z1);
        addBox(out, {xlo, T, zc2-T*0.5f}, {xhi, wallH-2*T, zc2+T*0.5f}, MemberType::Stud);
    }
    for (size_t i = startIdx; i < out.size(); ++i) out[i].zone = z;
}
} // anonymous namespace

// ---- JSON ロード（json.hpp の自作パーサを使用）------------------------------
FrameConfig loadConfig(const std::string& path) {
    FrameConfig cfg;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::printf("[config] %s not found — defaults: W=%.0f D=%.0f H=%.0f\n",
                    path.c_str(), cfg.width, cfg.depth, cfg.wallH);
        return cfg;
    }

    // ファイルサイズ上限（1 MB）
    f.seekg(0, std::ios::end);
    auto fileSize = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fileSize > 1 * 1024 * 1024) {
        std::printf("[config] %s too large (>1 MB) — defaults used\n", path.c_str());
        return cfg;
    }

    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());

    auto gf = [&](const char* key, float def) -> float {
        double v = jsNumber(s, key, -1.0);
        return v > 0 ? static_cast<float>(v) : def;
    };

    cfg.width     = gf("width",      cfg.width);
    cfg.depth     = gf("depth",      cfg.depth);
    cfg.wallH     = gf("wallHeight", cfg.wallH);
    cfg.rise      = gf("roofRise",   cfg.rise);
    cfg.studPitch = gf("studPitch",  cfg.studPitch);

    // 値域チェック＆クランプ
    auto clamp = [&](float& v, float lo, float hi, const char* name) {
        if (v < lo || v > hi) {
            std::printf("[config] %s=%.0f out of range [%.0f, %.0f] — clamped\n",
                        name, v, lo, hi);
            v = std::max(lo, std::min(hi, v));
        }
    };
    clamp(cfg.width,     500.0f,  30000.0f, "width");
    clamp(cfg.depth,     500.0f,  30000.0f, "depth");
    clamp(cfg.wallH,     1000.0f,  6000.0f, "wallHeight");
    clamp(cfg.rise,        0.0f,   5000.0f, "roofRise");
    clamp(cfg.studPitch,  100.0f,  1000.0f, "studPitch");

    // openings 配列を解析（各オブジェクト: type, x, width, sill, head）
    auto objs = jsArrayObjs(s, "openings");
    if (!objs.empty()) {
        cfg.frontOps.clear();
        for (const auto& obj : objs) {
            Opening o;
            o.x0   = static_cast<float>(jsNumber(obj, "x",     0.0));
            o.w    = static_cast<float>(jsNumber(obj, "width",  900.0));
            o.sill = static_cast<float>(jsNumber(obj, "sill",   0.0));
            o.head = static_cast<float>(jsNumber(obj, "head",   2000.0));
            // 開口の整合性チェック
            if (o.x0 < 0.0f)            o.x0 = 0.0f;
            if (o.w  < T)               o.w  = T;
            if (o.head <= o.sill)       o.head = o.sill + T;
            if (o.x0 + o.w > cfg.width) o.w  = cfg.width - o.x0;
            if (o.head > cfg.wallH)     o.head = cfg.wallH;
            cfg.frontOps.push_back(o);
        }
    }

    std::printf("[config] Loaded %s: W=%.0f D=%.0f H=%.0f rise=%.0f pitch=%.0f openings=%zu\n",
                path.c_str(), cfg.width, cfg.depth, cfg.wallH,
                cfg.rise, cfg.studPitch, cfg.frontOps.size());
    return cfg;
}

// ---- ①② AABB / 部材情報 ---------------------------------------------------
AABB memberBounds(const Member& m) {
    AABB a{{1e9f,1e9f,1e9f},{-1e9f,-1e9f,-1e9f}};
    float cs = std::cos(m.rotZ), sn = std::sin(m.rotZ);
    for (int sx : {-1,1}) for (int sy : {-1,1}) for (int sz : {-1,1}) {
        float lx = sx*m.half.x, ly = sy*m.half.y, lz = sz*m.half.z;
        float wx = lx*cs - ly*sn + m.center.x;
        float wy = lx*sn + ly*cs + m.center.y;
        float wz = lz + m.center.z;
        a.lo.x=std::min(a.lo.x,wx); a.lo.y=std::min(a.lo.y,wy); a.lo.z=std::min(a.lo.z,wz);
        a.hi.x=std::max(a.hi.x,wx); a.hi.y=std::max(a.hi.y,wy); a.hi.z=std::max(a.hi.z,wz);
    }
    return a;
}

std::string memberInfoTitle(const Member& m) {
    float dims[3] = {m.half.x*2.0f, m.half.y*2.0f, m.half.z*2.0f};
    std::sort(dims, dims + 3);
    const char* zs = zoneName(m.zone);
    const char* zSuffix = isWall(m.zone) ? " Wall" : "";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s | %s | %s%s | %.0fx%.0fmm | %.0fmm | %s",
                  m.id, typeName(m.type), zs, zSuffix,
                  dims[0], dims[1], dims[2], materialOf(m));
    return std::string(buf);
}

std::string memberInfoConsole(const Member& m) {
    float dims[3] = {m.half.x*2.0f, m.half.y*2.0f, m.half.z*2.0f};
    std::sort(dims, dims + 3);
    const char* zSuffix = isWall(m.zone) ? " Wall" : "";
    std::ostringstream os;
    os << "  ID:       " << m.id                              << "\n"
       << "  Type:     " << typeName(m.type)                  << "\n"
       << "  Zone:     " << zoneName(m.zone) << zSuffix       << "\n"
       << "  Section:  " << (int)dims[0] << " x " << (int)dims[1] << " mm" << "\n"
       << "  Length:   " << (int)dims[2] << " mm"             << "\n"
       << "  Material: " << materialOf(m)                     << "\n";
    return os.str();
}

// ---- フレーム生成 -----------------------------------------------------------
std::vector<Member> generateFrame(const FrameConfig& cfg) {
    const float W = cfg.width, D = cfg.depth, WALL_H = cfg.wallH, RISE = cfg.rise;
    const float hx = W*0.5f, hz = D*0.5f;

    std::vector<Member> m;
    buildWallAlongX(m,  hz-W4*0.5f, -hx, hx, WALL_H, cfg.frontOps, Zone::South);
    buildWallAlongX(m, -hz+W4*0.5f, -hx, hx, WALL_H, cfg.backOps,  Zone::North);
    buildWallAlongZ(m, -hx+W4*0.5f, -hz+W4, hz-W4,   WALL_H,       Zone::West);
    buildWallAlongZ(m,  hx-W4*0.5f, -hz+W4, hz-W4,   WALL_H,       Zone::East);

    // 天井根太 → Floor（天井面 = 1 階床扱い）
    const size_t joistStart = m.size();
    for (float z = -hz+M*0.5f; z < hz; z += M)
        addBox(m, {-hx, WALL_H, z-T*0.5f}, {hx, WALL_H+W6, z+T*0.5f}, MemberType::Joist);
    for (size_t i = joistStart; i < m.size(); ++i) m[i].zone = Zone::Floor;

    // 棟木・垂木 → Roof
    const size_t roofStart = m.size();
    const float ridgeH = WALL_H + RISE;
    addBox(m, {-T*0.5f, ridgeH-W6, -hz}, {T*0.5f, ridgeH, hz}, MemberType::Ridge);

    const float ang = std::atan2(RISE, hx);
    const float L   = std::sqrt(hx*hx + RISE*RISE);
    for (float z = -hz; z <= hz + 0.5f; z += M) {
        float zp = std::min(z, hz);
        Vec3 halfR{L*0.5f, W6*0.5f, T*0.5f};
        Member r1; r1.center={-hx*0.5f, WALL_H+RISE*0.5f, zp}; r1.half=halfR; r1.type=MemberType::Rafter; r1.rotZ= ang; m.push_back(r1);
        Member r2; r2.center={ hx*0.5f, WALL_H+RISE*0.5f, zp}; r2.half=halfR; r2.type=MemberType::Rafter; r2.rotZ=-ang; m.push_back(r2);
    }
    for (size_t i = roofStart; i < m.size(); ++i) m[i].zone = Zone::Roof;

    // ① ID を後付け（種別ごとの連番）
    std::map<MemberType, int> cnt;
    for (auto& mem : m) {
        int n = ++cnt[mem.type];
        std::snprintf(mem.id, sizeof(mem.id), "%s-%03d", idPrefix(mem.type), n);
    }

    return m;
}

// ---- メッシュ化 -------------------------------------------------------------
namespace {
Vec3 colorOf(MemberType t) {
    switch (t) {
        case MemberType::Plate:  return {0.86f, 0.56f, 0.30f};
        case MemberType::Stud:   return {0.82f, 0.73f, 0.55f};
        case MemberType::Header: return {0.74f, 0.36f, 0.30f};
        case MemberType::Joist:  return {0.53f, 0.66f, 0.82f};
        case MemberType::Rafter: return {0.52f, 0.76f, 0.60f};
        case MemberType::Ridge:  return {0.92f, 0.80f, 0.34f};
    }
    return {0.8f,0.8f,0.8f};
}
struct Face { Vec3 n; int c[4][3]; };
const Face FACES[6] = {
    {{ 1,0,0}, {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}}},
    {{-1,0,0}, {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}}},
    {{ 0,1,0}, {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}}},
    {{ 0,-1,0},{{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}}},
    {{ 0,0,1}, {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}},
    {{ 0,0,-1},{{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}}},
};
} // namespace

// scale > 1.0 で部材を中心から等倍拡大（ハイライト VAO 用）。
std::vector<float> buildMesh(const std::vector<Member>& members, float scale) {
    std::vector<float> v;
    v.reserve(members.size() * 36 * 9);
    for (const auto& mem : members) {
        Vec3 col = colorOf(mem.type);
        float cs = std::cos(mem.rotZ), sn = std::sin(mem.rotZ);
        auto place = [&](Vec3 lo) -> Vec3 {
            return Vec3{lo.x*cs - lo.y*sn + mem.center.x,
                        lo.x*sn + lo.y*cs + mem.center.y,
                        lo.z + mem.center.z};
        };
        auto rotN = [&](Vec3 n) -> Vec3 {
            return {n.x*cs - n.y*sn, n.x*sn + n.y*cs, n.z};
        };
        for (const auto& f : FACES) {
            Vec3 n = normalize(rotN(f.n));
            Vec3 p[4];
            for (int i = 0; i < 4; ++i)
                p[i] = place({f.c[i][0]*mem.half.x*scale,
                               f.c[i][1]*mem.half.y*scale,
                               f.c[i][2]*mem.half.z*scale});
            const int idx[6] = {0,1,2,0,2,3};
            for (int k : idx)
                v.insert(v.end(), {p[k].x,p[k].y,p[k].z, n.x,n.y,n.z, col.x,col.y,col.z});
        }
    }
    return v;
}

std::vector<float> buildGrid(float extent, float step) {
    std::vector<float> v;
    for (float x = -extent; x <= extent+0.5f; x += step)
        v.insert(v.end(), {x,0.0f,-extent, x,0.0f,extent});
    for (float z = -extent; z <= extent+0.5f; z += step)
        v.insert(v.end(), {-extent,0.0f,z, extent,0.0f,z});
    return v;
}

// ---- ③ 寸法線 + ストロークフォント ----------------------------------------
namespace {
inline void seg(std::vector<float>& v, Vec3 a, Vec3 b) {
    v.insert(v.end(), {a.x,a.y,a.z, b.x,b.y,b.z});
}

// 7セグメント風の数字と 'M' ' ' を GL_LINES で描く。col = 文字送り量（文字幅単位）。
void glyph(std::vector<float>& v, char ch, Vec3 org, Vec3 adv, Vec3 up, float sz, float col) {
    auto P = [&](float gx, float gy) -> Vec3 {
        return org + adv * ((gx + col) * sz) + up * (gy * sz);
    };
    auto S = [&](float ax, float ay, float bx, float by) { seg(v, P(ax,ay), P(bx,by)); };
    switch (ch) {
        case '0': S(0,0,1,0);S(1,0,1,1);S(1,1,0,1);S(0,1,0,0); break;
        case '1': S(1,0,1,1); break;
        case '2': S(0,1,1,1);S(1,1,1,.5f);S(1,.5f,0,.5f);S(0,.5f,0,0);S(0,0,1,0); break;
        case '3': S(0,1,1,1);S(1,1,1,0);S(0,0,1,0);S(0,.5f,1,.5f); break;
        case '4': S(0,1,0,.5f);S(0,.5f,1,.5f);S(1,1,1,0); break;
        case '5': S(1,1,0,1);S(0,1,0,.5f);S(0,.5f,1,.5f);S(1,.5f,1,0);S(1,0,0,0); break;
        case '6': S(1,1,0,1);S(0,1,0,0);S(0,0,1,0);S(1,0,1,.5f);S(1,.5f,0,.5f); break;
        case '7': S(0,1,1,1);S(1,1,1,0); break;
        case '8': S(0,0,1,0);S(1,0,1,1);S(1,1,0,1);S(0,1,0,0);S(0,.5f,1,.5f); break;
        case '9': S(1,0,1,1);S(1,1,0,1);S(0,1,0,.5f);S(0,.5f,1,.5f); break;
        case 'M': S(0,0,0,1);S(0,1,.5f,.45f);S(.5f,.45f,1,1);S(1,1,1,0); break;
        case ' ': default: break;
    }
}

void strokeText(std::vector<float>& v, const std::string& s,
                Vec3 org, Vec3 adv, Vec3 up, float sz) {
    float col = 0.0f;
    for (char c : s) { glyph(v, c, org, adv, up, sz, col); col += 1.4f; }
}

std::string mmStr(float mm) {
    std::ostringstream os; os << std::llround(mm); return os.str();
}
} // namespace (stroke font)

std::vector<float> buildDimLines(float W, float D) {
    std::vector<float> v;
    const float hx=W*0.5f, hz=D*0.5f;
    const float off=520.0f, ext=130.0f, tick=110.0f, ty=2.0f;

    // ── 幅方向 (X)：前面の外側 ──
    float zd = hz + off;
    seg(v, {-hx,ty,zd},      { hx,ty,zd});               // 寸法線
    seg(v, {-hx,ty,hz},      {-hx,ty,zd+ext});            // 補助線
    seg(v, { hx,ty,hz},      { hx,ty,zd+ext});
    seg(v, {-hx-tick*0.5f,ty,zd-tick*0.5f}, {-hx+tick*0.5f,ty,zd+tick*0.5f}); // 端末チック
    seg(v, { hx-tick*0.5f,ty,zd-tick*0.5f}, { hx+tick*0.5f,ty,zd+tick*0.5f});
    {   // 数値ラベル「3640 MM」— 送り +X、高さ +Y で正面から読める位置に配置
        std::string label = mmStr(W) + " MM";
        float sz = 280.0f, tw = label.size() * 1.4f * sz;
        strokeText(v, label, {-tw*0.5f, 40.0f, zd+ext+150.0f}, {1,0,0}, {0,1,0}, sz);
    }

    // ── 奥行方向 (Z)：右側の外側 ──
    float xd = hx + off;
    seg(v, {xd,ty,-hz},      {xd,ty, hz});
    seg(v, {hx,ty,-hz},      {xd+ext,ty,-hz});
    seg(v, {hx,ty, hz},      {xd+ext,ty, hz});
    seg(v, {xd-tick*0.5f,ty,-hz-tick*0.5f}, {xd+tick*0.5f,ty,-hz+tick*0.5f});
    seg(v, {xd-tick*0.5f,ty, hz-tick*0.5f}, {xd+tick*0.5f,ty, hz+tick*0.5f});
    {   // 数値ラベル「2730 MM」— 送り -Z で視点から正しい向きに
        std::string label = mmStr(D) + " MM";
        float sz = 280.0f, tw = label.size() * 1.4f * sz;
        strokeText(v, label, {xd+ext+150.0f, 40.0f, tw*0.5f}, {0,0,-1}, {0,1,0}, sz);
    }
    return v;
}

std::string billOfMaterials(const std::vector<Member>& members) {
    std::map<MemberType,int>   count;
    std::map<MemberType,float> len;
    for (const auto& m : members) {
        float longest = std::max({m.half.x, m.half.y, m.half.z}) * 2.0f;
        count[m.type] += 1;
        len[m.type]   += longest / 1000.0f;
    }
    auto name = [](MemberType t) -> const char* {
        switch (t) {
            case MemberType::Plate:  return "Plate (土台/上枠)";
            case MemberType::Stud:   return "Stud (スタッド)";
            case MemberType::Header: return "Header (まぐさ)";
            case MemberType::Joist:  return "Joist (根太)";
            case MemberType::Rafter: return "Rafter (垂木)";
            case MemberType::Ridge:  return "Ridge (棟木)";
        }
        return "?";
    };
    int totalN=0; float totalL=0.0f;
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    os << "==== Bill of Materials ====\n";
    for (auto t : {MemberType::Plate, MemberType::Stud, MemberType::Header,
                   MemberType::Joist, MemberType::Rafter, MemberType::Ridge}) {
        os << "  " << std::left << std::setw(22) << name(t)
           << std::right << std::setw(4) << count[t] << " pcs   "
           << std::setw(7) << len[t] << " m\n";
        totalN += count[t]; totalL += len[t];
    }
    os << "  " << std::string(40,'-') << "\n";
    os << "  " << std::left << std::setw(22) << "Total"
       << std::right << std::setw(4) << totalN << " pcs   " << std::setw(7) << totalL << " m\n";
    return os.str();
}

// ⑤ CSV エクスポート
bool csvExport(const std::vector<Member>& members, const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "ID,Type,Zone,Material,Section_W_mm,Section_H_mm,Length_mm\n";
    for (const auto& m : members) {
        float dims[3] = {m.half.x*2.0f, m.half.y*2.0f, m.half.z*2.0f};
        std::sort(dims, dims+3);
        const char* zSuffix = isWall(m.zone) ? " Wall" : "";
        f << m.id << ","
          << typeName(m.type) << ","
          << zoneName(m.zone) << zSuffix << ","
          << materialOf(m) << ","
          << (int)(dims[0]+0.5f) << ","
          << (int)(dims[1]+0.5f) << ","
          << (int)(dims[2]+0.5f) << "\n";
    }
    return true;
}

Vec3 frameCenter(const FrameConfig& cfg) {
    return {0.0f, (cfg.wallH + cfg.rise) * 0.42f, 0.0f};
}

} // namespace fl
