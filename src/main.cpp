// FrameLens — 2x4 枠組フレームの 3D ビューア
// C++17 / OpenGL 3.3 Core / GLFW / GLEW
//
// 操作:
//   マウス左クリック（ドラッグなし）… 部材クリック選択 + 情報表示
//   マウス左ドラッグ               … 回転（オービット）
//   スクロール                     … ズーム
//   W                              … ソリッド / ワイヤーフレーム切替
//   G                              … 参照グリッド 表示切替
//   D                              … 寸法線 表示切替
//   C                              … CSV エクスポート（framelens_export.csv）
//   P                              … スクリーンショット保存（screenshot.ppm）
//   R                              … 視点リセット
//   ESC                            … 終了
//
// ヘッドレス書き出し:
//   framelens --shot out.ppm [--size 1280x800] [--yaw 0.9] [--pitch 0.35]
// JSON 設定読み込み:
//   framelens [--config house.json]
//
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>

#include "linalg.hpp"
#include "camera.hpp"
#include "framing.hpp"
#include "shaders.hpp"

using namespace fl;

// ---- コールバック用の共有状態 -----------------------------------------------
struct AppState {
    OrbitCamera cam, home;
    bool   dragging    = false;
    double lastX = 0, lastY = 0;
    bool   wire        = false;
    bool   grid        = true;
    bool   dims        = true;
    int    selectedMember = -1;
    bool   pendingPick      = false;
    bool   pendingCsvExport = false;
    bool   pendingShot      = false;
    double pickX = 0, pickY = 0;
};

static void mouseButtonCB(GLFWwindow* w, int button, int action, int) {
    auto* s = static_cast<AppState*>(glfwGetWindowUserPointer(w));
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            s->dragging = true;
            glfwGetCursorPos(w, &s->lastX, &s->lastY);
            s->pickX = s->lastX; s->pickY = s->lastY;
        } else {
            double cx, cy;
            glfwGetCursorPos(w, &cx, &cy);
            if (std::fabs(cx - s->pickX) < 5.0 && std::fabs(cy - s->pickY) < 5.0)
                s->pendingPick = true;
            s->dragging = false;
        }
    }
}
static void cursorPosCB(GLFWwindow* w, double x, double y) {
    auto* s = static_cast<AppState*>(glfwGetWindowUserPointer(w));
    if (s->dragging) {
        s->cam.orbit(static_cast<float>(x - s->lastX) * 0.008f,
                    -static_cast<float>(y - s->lastY) * 0.008f);
        s->lastX = x; s->lastY = y;
    }
}
static void scrollCB(GLFWwindow* w, double, double dy) {
    static_cast<AppState*>(glfwGetWindowUserPointer(w))->cam.zoom(dy > 0 ? 0.9f : 1.111f);
}
static void keyCB(GLFWwindow* w, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    auto* s = static_cast<AppState*>(glfwGetWindowUserPointer(w));
    switch (key) {
        case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(w, 1);  break;
        case GLFW_KEY_W:      s->wire = !s->wire;              break;
        case GLFW_KEY_G:      s->grid = !s->grid;              break;
        case GLFW_KEY_D:      s->dims = !s->dims;              break;
        case GLFW_KEY_C:      s->pendingCsvExport = true;      break;
        case GLFW_KEY_P:      s->pendingShot      = true;      break;
        case GLFW_KEY_R:      s->cam = s->home;                break;
        default: break;
    }
}

// ---- PPM 書き出し -----------------------------------------------------------
static bool writePPM(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h-1; y >= 0; --y)
        f.write(reinterpret_cast<char*>(&px[static_cast<size_t>(y)*w*3]),
                static_cast<std::streamsize>(w)*3);
    return true;
}

// ⑧ BBox の 12 エッジ（GL_LINES: 24 頂点 × 3 float）
static std::vector<float> makeBBoxLines(const AABB& b) {
    const float xs[2]={b.lo.x,b.hi.x}, ys[2]={b.lo.y,b.hi.y}, zs[2]={b.lo.z,b.hi.z};
    std::vector<float> v;
    v.reserve(72);
    for (int i : {0,1}) for (int j : {0,1}) {
        v.insert(v.end(), {xs[0],ys[i],zs[j], xs[1],ys[i],zs[j]}); // X エッジ
        v.insert(v.end(), {xs[i],ys[0],zs[j], xs[i],ys[1],zs[j]}); // Y エッジ
        v.insert(v.end(), {xs[i],ys[j],zs[0], xs[i],ys[j],zs[1]}); // Z エッジ
    }
    return v;
}

int main(int argc, char** argv) {
    // ---- 引数 ---------------------------------------------------------------
    std::string shot;
    std::string configPath = "house.json";
    int   winW = 1280, winH = 800;
    int   selectArg = -1;           // --select N: ヘッドレス時に部材をプリセレクト
    float argYaw = 0.9f, argPitch = 0.35f;
    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--shot")   && i+1<argc) shot       = argv[++i];
        else if ((!std::strcmp(argv[i], "--in") ||
                  !std::strcmp(argv[i], "--config")) && i+1<argc) configPath = argv[++i];
        else if (!std::strcmp(argv[i], "--select") && i+1<argc) selectArg = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--size")   && i+1<argc) std::sscanf(argv[++i], "%dx%d", &winW, &winH);
        else if (!std::strcmp(argv[i], "--yaw")    && i+1<argc) argYaw   = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--pitch")  && i+1<argc) argPitch = static_cast<float>(std::atof(argv[++i]));
    }

    // ---- 設定 + ジオメトリ生成 ----------------------------------------------
    FrameConfig cfg   = loadConfig(configPath);
    std::vector<Member> frame = generateFrame(cfg);
    std::vector<float>  mesh  = buildMesh(frame);
    std::vector<float>  grid  = buildGrid(4000.0f, cfg.studPitch);
    std::vector<float>  dimv  = buildDimLines(cfg.width, cfg.depth);

    std::printf("Members: %zu\n%s\n", frame.size(), billOfMaterials(frame).c_str());

    // ---- GLFW / OpenGL 初期化 -----------------------------------------------
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    if (!shot.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(winW, winH,
        "FrameLens — 2x4 Framing Viewer  |  Click member to inspect",
        nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::fprintf(stderr, "glewInit failed\n"); return 1; }
    glGetError();
    std::printf("OpenGL %s / GLSL %s\n",
                glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    // ---- メッシュ VAO/VBO (GL_STATIC_DRAW: 選択時も変更しない) -------------
    const GLsizei meshVerts = static_cast<GLsizei>(mesh.size() / 9);
    GLuint meshVAO, meshVBO;
    glGenVertexArrays(1, &meshVAO); glGenBuffers(1, &meshVBO);
    glBindVertexArray(meshVAO);
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.size()*sizeof(float)),
                 mesh.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0);            glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);

    // ---- グリッド VAO/VBO ---------------------------------------------------
    const GLsizei gridVerts = static_cast<GLsizei>(grid.size() / 3);
    GLuint gridVAO, gridVBO;
    glGenVertexArrays(1, &gridVAO); glGenBuffers(1, &gridVBO);
    glBindVertexArray(gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(grid.size()*sizeof(float)),
                 grid.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);

    // ---- 寸法線 VAO/VBO -----------------------------------------------------
    const GLsizei dimVerts = static_cast<GLsizei>(dimv.size() / 3);
    GLuint dimVAO, dimVBO;
    glGenVertexArrays(1, &dimVAO); glGenBuffers(1, &dimVBO);
    glBindVertexArray(dimVAO);
    glBindBuffer(GL_ARRAY_BUFFER, dimVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dimv.size()*sizeof(float)),
                 dimv.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);

    // ---- ハイライト VAO/VBO（選択部材1本を 1.04x スケールで別描画）---------
    GLuint hlVAO, hlVBO;
    GLsizei hlVerts = 0;
    glGenVertexArrays(1, &hlVAO); glGenBuffers(1, &hlVBO);
    glBindVertexArray(hlVAO);
    glBindBuffer(GL_ARRAY_BUFFER, hlVBO);
    glBufferData(GL_ARRAY_BUFFER, 36*9*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0);            glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);

    // ---- ⑧ BBox VAO/VBO (選択時に glBufferData で更新) ---------------------
    GLuint bboxVAO, bboxVBO;
    glGenVertexArrays(1, &bboxVAO); glGenBuffers(1, &bboxVBO);
    glBindVertexArray(bboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, bboxVBO);
    glBufferData(GL_ARRAY_BUFFER, 72*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    GLuint modelProg = linkProgram(MODEL_VS, MODEL_FS);
    GLuint lineProg  = linkProgram(LINE_VS, LINE_FS);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    AppState state;
    state.cam.target = frameCenter(cfg);
    state.cam.yaw    = argYaw;
    state.cam.pitch  = argPitch;
    state.cam.dist   = 8200.0f;
    state.home       = state.cam;
    glfwSetWindowUserPointer(win, &state);
    glfwSetMouseButtonCallback(win, mouseButtonCB);
    glfwSetCursorPosCallback(win, cursorPosCB);
    glfwSetScrollCallback(win, scrollCB);
    glfwSetKeyCallback(win, keyCB);

    // ---- 描画ラムダ ---------------------------------------------------------
    auto drawScene = [&](int fbW, int fbH) {
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.11f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float aspect = fbH > 0 ? static_cast<float>(fbW)/fbH : 1.0f;
        Mat4 proj = perspective(0.785398f, aspect, 100.0f, 60000.0f);
        Mat4 view = state.cam.view();
        Mat4 mvp  = proj * view;
        Vec3 ld   = normalize(state.cam.eye() - state.cam.target + Vec3{0.2f,0.5f,0.1f});

        if (state.grid) {
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg,"uMVP"),1,GL_FALSE,mvp.data());
            glUniform3f(glGetUniformLocation(lineProg,"uColor"), 0.28f,0.30f,0.34f);
            glBindVertexArray(gridVAO);
            glDrawArrays(GL_LINES, 0, gridVerts);
        }
        if (state.dims) {
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg,"uMVP"),1,GL_FALSE,mvp.data());
            glUniform3f(glGetUniformLocation(lineProg,"uColor"), 0.95f,0.92f,0.45f);
            glBindVertexArray(dimVAO);
            glDrawArrays(GL_LINES, 0, dimVerts);
        }
        // ⑧ 選択部材の BoundingBox（白線）
        if (state.selectedMember >= 0) {
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg,"uMVP"),1,GL_FALSE,mvp.data());
            glUniform3f(glGetUniformLocation(lineProg,"uColor"), 1.0f,0.95f,0.55f);
            glBindVertexArray(bboxVAO);
            glDrawArrays(GL_LINES, 0, 24);
        }

        glUseProgram(modelProg);
        GLint locMVP  = glGetUniformLocation(modelProg,"uMVP");
        GLint locLD   = glGetUniformLocation(modelProg,"uLightDir");
        GLint locWire = glGetUniformLocation(modelProg,"uWire");
        GLint locHL   = glGetUniformLocation(modelProg,"uHighlight");
        glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp.data());
        glUniform3f(locLD, ld.x,ld.y,ld.z);
        glUniform1f(locWire, state.wire ? 1.0f : 0.0f);
        glUniform1f(locHL, 0.0f);
        glPolygonMode(GL_FRONT_AND_BACK, state.wire ? GL_LINE : GL_FILL);
        glBindVertexArray(meshVAO);
        glDrawArrays(GL_TRIANGLES, 0, meshVerts);
        // 選択部材を 1.04x スケール + 黄色で重ねる
        if (hlVerts > 0) {
            glUniform1f(locHL, 1.0f);
            glBindVertexArray(hlVAO);
            glDrawArrays(GL_TRIANGLES, 0, hlVerts);
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glBindVertexArray(0);
    };

    // ---- ヘッドレスモード ---------------------------------------------------
    if (!shot.empty()) {
        // --select N でプリセレクト（スクリーンショット用）
        if (selectArg >= 0 && selectArg < (int)frame.size()) {
            auto hm = buildMesh({frame[selectArg]}, 1.04f);
            hlVerts = static_cast<GLsizei>(hm.size() / 9);
            glBindBuffer(GL_ARRAY_BUFFER, hlVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(hm.size()*sizeof(float)),
                         hm.data(), GL_DYNAMIC_DRAW);
            auto blines = makeBBoxLines(memberBounds(frame[selectArg]));
            glBindBuffer(GL_ARRAY_BUFFER, bboxVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(blines.size()*sizeof(float)),
                         blines.data(), GL_DYNAMIC_DRAW);
            state.selectedMember = selectArg;
            std::printf("[select] %s\n%s",
                        frame[selectArg].id,
                        memberInfoConsole(frame[selectArg]).c_str());
        }
        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        drawScene(fbW, fbH);
        glFinish();
        if (writePPM(shot, fbW, fbH)) std::printf("wrote %s (%dx%d)\n", shot.c_str(), fbW, fbH);
        else std::fprintf(stderr, "failed to write %s\n", shot.c_str());
        glfwDestroyWindow(win);
        glfwTerminate();
        return 0;
    }

    // ---- インタラクティブ・ループ -------------------------------------------
    double prevTime   = glfwGetTime();
    int    frameCount = 0;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // ⑫ FPS（1 秒ごとにタイトル更新・部材未選択時のみ）
        ++frameCount;
        double now = glfwGetTime();
        if (now - prevTime >= 1.0) {
            if (state.selectedMember < 0) {
                char title[160];
                std::snprintf(title, sizeof(title),
                    "FrameLens — 2x4 Framing Viewer  |  %.0f FPS  |  %zu members  |  Click to select",
                    frameCount / (now - prevTime), frame.size());
                glfwSetWindowTitle(win, title);
            }
            frameCount = 0;
            prevTime   = now;
        }

        // ① Ray Picking
        if (state.pendingPick) {
            state.pendingPick = false;
            int fbW, fbH;
            glfwGetFramebufferSize(win, &fbW, &fbH);
            float aspect = fbH > 0 ? static_cast<float>(fbW)/fbH : 1.0f;
            float ndcX = static_cast<float>(2.0*state.pickX/fbW - 1.0);
            float ndcY = static_cast<float>(1.0 - 2.0*state.pickY/fbH);

            Vec3 eye = state.cam.eye();
            Vec3 ray = screenRay(eye, state.cam.target, 0.785398f, aspect, ndcX, ndcY);

            int   best  = -1;
            float bestT = 1e9f;
            for (int i = 0; i < (int)frame.size(); ++i) {
                AABB b = memberBounds(frame[i]);
                float t = rayAABB(eye, ray, b.lo, b.hi);
                if (t >= 0.0f && t < bestT) { bestT = t; best = i; }
            }

            if (best != state.selectedMember) {
                state.selectedMember = best;
                if (best >= 0) {
                    // ② 選択部材1本を 1.04x スケールで hlVBO にアップロード
                    auto hm = buildMesh({frame[best]}, 1.04f);
                    hlVerts = static_cast<GLsizei>(hm.size() / 9);
                    glBindBuffer(GL_ARRAY_BUFFER, hlVBO);
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(hm.size()*sizeof(float)),
                                 hm.data(), GL_DYNAMIC_DRAW);
                    // ⑧ BBox 更新
                    auto blines = makeBBoxLines(memberBounds(frame[best]));
                    glBindBuffer(GL_ARRAY_BUFFER, bboxVBO);
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(blines.size()*sizeof(float)),
                                 blines.data(), GL_DYNAMIC_DRAW);
                    // ① タイトル + コンソール
                    std::string title = "FrameLens | " + memberInfoTitle(frame[best]);
                    glfwSetWindowTitle(win, title.c_str());
                    std::printf("[select]\n%s", memberInfoConsole(frame[best]).c_str());
                } else {
                    hlVerts = 0;
                    glfwSetWindowTitle(win,
                        "FrameLens — 2x4 Framing Viewer  |  Click member to inspect");
                }
            }
        }

        // ⑤ CSV エクスポート（C キー）
        if (state.pendingCsvExport) {
            state.pendingCsvExport = false;
            const char* csvPath = "framelens_export.csv";
            if (csvExport(frame, csvPath))
                std::printf("[export] Saved %s (%zu members)\n", csvPath, frame.size());
            else
                std::fprintf(stderr, "[export] Failed to write %s\n", csvPath);
        }

        // P キー → スクリーンショット
        if (state.pendingShot) {
            state.pendingShot = false;
            int fbW, fbH;
            glfwGetFramebufferSize(win, &fbW, &fbH);
            if (writePPM("screenshot.ppm", fbW, fbH))
                std::printf("[screenshot] Saved screenshot.ppm (%dx%d)\n", fbW, fbH);
        }

        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        drawScene(fbW, fbH);
        glfwSwapBuffers(win);
    }

    glDeleteVertexArrays(1, &meshVAO); glDeleteBuffers(1, &meshVBO);
    glDeleteVertexArrays(1, &gridVAO); glDeleteBuffers(1, &gridVBO);
    glDeleteVertexArrays(1, &dimVAO);  glDeleteBuffers(1, &dimVBO);
    glDeleteVertexArrays(1, &hlVAO);   glDeleteBuffers(1, &hlVBO);
    glDeleteVertexArrays(1, &bboxVAO); glDeleteBuffers(1, &bboxVBO);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
