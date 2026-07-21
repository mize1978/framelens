// framing.hpp — 2x4（枠組壁工法）フレームのデータモデルとジェネレータ。
// すべて実寸(mm)で構築する。部材=直方体で、屋根垂木のみ Z 軸まわりに回転させる。
#pragma once
#include "linalg.hpp"
#include <vector>
#include <string>

namespace fl {

enum class MemberType { Plate, Stud, Header, Joist, Rafter, Ridge };

// ② 建物内での部材の位置（壁面・床・屋根）
enum class Zone { South, North, East, West, Floor, Roof };

// 1 本の構造材。
struct Member {
    Vec3 center;
    Vec3 half;
    MemberType type;
    float rotZ = 0.0f;  // ラジアン。0 なら軸並行の直方体。
    Zone  zone = Zone::South;
    char  id[8] = {};   // "S-014\0" — prefix + 3桁番号 (最大7文字)
};

// 開口部（窓・ドア）。壁の左端からの距離 x0、幅 w、下端 sill、上端 head（mm）。
struct Opening {
    float x0, w, sill, head;
};

// house.json から読み込める建物形状の設定。
struct FrameConfig {
    float width      = 3640.0f;
    float depth      = 2730.0f;
    float wallH      = 2400.0f;
    float rise       = 900.0f;
    float studPitch  = 455.0f;   // スタッドピッチ（グリッド間隔にも使用）
    std::vector<Opening> frontOps = {
        {700.0f, 900.0f, 0.0f, 2000.0f},
        {2400.0f, 1200.0f, 900.0f, 2000.0f}
    };
    std::vector<Opening> backOps;
};

// house.json から FrameConfig を読む（ファイルなければデフォルト値）。
// JSON キー: width / depth / wallHeight / studPitch / roofRise / openings[]
FrameConfig loadConfig(const std::string& path);

// ①② 世界座標での軸並行 AABB（回転部材は8頂点から計算）。
struct AABB { Vec3 lo, hi; };
AABB memberBounds(const Member& m);

// ① 部材情報をウィンドウタイトル用の1行文字列で返す。
std::string memberInfoTitle(const Member& m);

// ① 部材情報をコンソール向けの複数行文字列で返す。
std::string memberInfoConsole(const Member& m);

// 建物 1 棟分の全部材を生成する（ID・Zone も付与）。
std::vector<Member> generateFrame(const FrameConfig& cfg = {});

// 部材リストから描画用の頂点バッファを構築（interleave: pos3 / normal3 / color3）。
// scale > 1.0 で部材を中心から拡大（ハイライト用に 1.04f など）。
std::vector<float> buildMesh(const std::vector<Member>& members, float scale = 1.0f);

// 床の参照グリッド（GL_LINES 用の頂点バッファ pos3）。
std::vector<float> buildGrid(float extent, float step);

// ③ 寸法線（GL_LINES 用 pos3 バッファ）。
std::vector<float> buildDimLines(float W, float D);

// 拾い出し（部材種別ごとの本数・総材長）を人間可読な文字列で返す。
std::string billOfMaterials(const std::vector<Member>& members);

// ⑤ 部材リストを CSV ファイルに書き出す。成功で true を返す。
bool csvExport(const std::vector<Member>& members, const std::string& path);

// バウンディングから適当な注視点（建物中心）を返す。
Vec3 frameCenter(const FrameConfig& cfg = {});

} // namespace fl
