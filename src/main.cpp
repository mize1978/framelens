// FrameLens — 2x4 枠組フレームの 3D ビューア
// C++17 / OpenGL 3.3 Core (ネイティブ) · WebGL2/GLES3 (Web, Emscripten)
//
// 操作:
//   マウス左クリック（ドラッグなし）… 部材クリック選択 + 情報表示
//   マウス左ドラッグ               … 回転（オービット）
//   スクロール                     … ズーム
//   W                              … ソリッド / ワイヤーフレーム切替
//   G                              … 参照グリッド 表示切替
//   D                              … 寸法線 表示切替
//   C                              … CSV エクスポート（Web はブラウザ DL）
//   P                              … スクリーンショット保存（ネイティブのみ）
//   R                              … 視点リセット
//   ESC                            … 終了（ネイティブのみ）
//
// ヘッドレス書き出し（ネイティブ）:
//   framelens --shot out.ppm [--size 1280x800] [--yaw 0.9] [--pitch 0.35]
// JSON 設定読み込み:
//   framelens [--config house.json]
//
#include "gl.hpp"
#include <GLFW/glfw3.h>
#ifdef __EMSCRIPTEN__
  #include <emscripten.h>
  #include <emscripten/html5.h>
#endif
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

#ifdef __EMSCRIPTEN__
// MEMFS に書いたファイルをブラウザのダウンロードとして落とす。
EM_JS(void, browserDownload, (const char* path, const char* filename, const char* mime), {
    var p = UTF8ToString(path), name = UTF8ToString(filename), m = UTF8ToString(mime);
    try {
        var data = FS.readFile(p);
        var blob = new Blob([data], { type: m });
        var url  = URL.createObjectURL(blob);
        var a    = document.createElement('a');
        a.href = url; a.download = name;
        document.body.appendChild(a); a.click();
        document.body.removeChild(a); URL.revokeObjectURL(url);
    } catch (e) { console.error('download failed', e); }
});
#endif

// ---- PPM 書き出し（ネイティブのスクリーンショット/--shot 用）---------------
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

// ---- アプリ状態（ヒープ確保：Web のメインループはコールバック方式のため）----
struct App {
    GLFWwindow* win = nullptr;
    AppState state;
    std::vector<Member> frame;

    GLuint  meshVAO=0, gridVAO=0, dimVAO=0, hlVAO=0, hlVBO=0, bboxVAO=0, bboxVBO=0;
    GLsizei meshVerts=0, gridVerts=0, dimVerts=0, hlVerts=0;
    GLuint  modelProg=0, lineProg=0;

    double prevTime=0; int frameCount=0;

    // ---- 描画 -------------------------------------------------------------
    void draw(int fbW, int fbH) {
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
#ifndef __EMSCRIPTEN__
        // glPolygonMode は GLES3/WebGL2 に無い。Web ではシェーダ側の uWire 着色で表現。
        glPolygonMode(GL_FRONT_AND_BACK, state.wire ? GL_LINE : GL_FILL);
#endif
        glBindVertexArray(meshVAO);
        glDrawArrays(GL_TRIANGLES, 0, meshVerts);
        if (hlVerts > 0) {
            glUniform1f(locHL, 1.0f);
            glBindVertexArray(hlVAO);
            glDrawArrays(GL_TRIANGLES, 0, hlVerts);
        }
#ifndef __EMSCRIPTEN__
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
        glBindVertexArray(0);
    }

    // ---- 1 フレーム分の更新（旧インタラクティブループの中身）--------------
    void tick() {
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
                    auto hm = buildMesh({frame[best]}, 1.04f);
                    hlVerts = static_cast<GLsizei>(hm.size() / 9);
                    glBindBuffer(GL_ARRAY_BUFFER, hlVBO);
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(hm.size()*sizeof(float)),
                                 hm.data(), GL_DYNAMIC_DRAW);
                    auto blines = makeBBoxLines(memberBounds(frame[best]));
                    glBindBuffer(GL_ARRAY_BUFFER, bboxVBO);
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(blines.size()*sizeof(float)),
                                 blines.data(), GL_DYNAMIC_DRAW);
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
            if (csvExport(frame, csvPath)) {
                std::printf("[export] Saved %s (%zu members)\n", csvPath, frame.size());
#ifdef __EMSCRIPTEN__
                browserDownload(csvPath, csvPath, "text/csv");
#endif
            } else {
                std::fprintf(stderr, "[export] Failed to write %s\n", csvPath);
            }
        }

        // P キー → スクリーンショット（ネイティブのみ）
        if (state.pendingShot) {
            state.pendingShot = false;
#ifndef __EMSCRIPTEN__
            int fbW, fbH;
            glfwGetFramebufferSize(win, &fbW, &fbH);
            if (writePPM("screenshot.ppm", fbW, fbH))
                std::printf("[screenshot] Saved screenshot.ppm (%dx%d)\n", fbW, fbH);
#endif
        }

        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        draw(fbW, fbH);
        glfwSwapBuffers(win);
    }
};

static App* g_app = nullptr;

int main(int argc, char** argv) {
    // ---- 引数 ---------------------------------------------------------------
    std::string shot;
    std::string configPath = "house.json";
    int   winW = 1280, winH = 800;
    int   selectArg = -1;
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
#ifdef __EMSCRIPTEN__
    // WebGL2 / GLES3 コンテキスト
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 4);
    if (!shot.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(winW, winH,
        "FrameLens — 2x4 Framing Viewer  |  Click member to inspect",
        nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

#ifndef __EMSCRIPTEN__
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::fprintf(stderr, "glewInit failed\n"); return 1; }
#endif
    glGetError();
    std::printf("OpenGL %s / GLSL %s\n",
                glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    // ---- アプリ状態（ヒープ）------------------------------------------------
    App* app = new App();
    g_app = app;
    app->win   = win;
    app->frame = std::move(frame);

    // ---- メッシュ VAO/VBO ---------------------------------------------------
    app->meshVerts = static_cast<GLsizei>(mesh.size() / 9);
    GLuint meshVBO;
    glGenVertexArrays(1, &app->meshVAO); glGenBuffers(1, &meshVBO);
    glBindVertexArray(app->meshVAO);
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.size()*sizeof(float)),
                 mesh.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0);            glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);

    // ---- グリッド VAO/VBO ---------------------------------------------------
    app->gridVerts = static_cast<GLsizei>(grid.size() / 3);
    GLuint gridVBO;
    glGenVertexArrays(1, &app->gridVAO); glGenBuffers(1, &gridVBO);
    glBindVertexArray(app->gridVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(grid.size()*sizeof(float)),
                 grid.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);

    // ---- 寸法線 VAO/VBO -----------------------------------------------------
    app->dimVerts = static_cast<GLsizei>(dimv.size() / 3);
    GLuint dimVBO;
    glGenVertexArrays(1, &app->dimVAO); glGenBuffers(1, &dimVBO);
    glBindVertexArray(app->dimVAO);
    glBindBuffer(GL_ARRAY_BUFFER, dimVBO);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dimv.size()*sizeof(float)),
                 dimv.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);

    // ---- ハイライト VAO/VBO -------------------------------------------------
    glGenVertexArrays(1, &app->hlVAO); glGenBuffers(1, &app->hlVBO);
    glBindVertexArray(app->hlVAO);
    glBindBuffer(GL_ARRAY_BUFFER, app->hlVBO);
    glBufferData(GL_ARRAY_BUFFER, 36*9*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)0);            glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,9*sizeof(float),(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);

    // ---- ⑧ BBox VAO/VBO -----------------------------------------------------
    glGenVertexArrays(1, &app->bboxVAO); glGenBuffers(1, &app->bboxVBO);
    glBindVertexArray(app->bboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, app->bboxVBO);
    glBufferData(GL_ARRAY_BUFFER, 72*sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void*)0); glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    app->modelProg = linkProgram(MODEL_VS, MODEL_FS);
    app->lineProg  = linkProgram(LINE_VS, LINE_FS);

    glEnable(GL_DEPTH_TEST);
#ifndef __EMSCRIPTEN__
    glEnable(GL_MULTISAMPLE); // GLES3 は既定 FBO の MSAA を context 属性で行うため不要
#endif

    app->state.cam.target = frameCenter(cfg);
    app->state.cam.yaw    = argYaw;
    app->state.cam.pitch  = argPitch;
    app->state.cam.dist   = 8200.0f;
    app->state.home       = app->state.cam;
    glfwSetWindowUserPointer(win, &app->state);
    glfwSetMouseButtonCallback(win, mouseButtonCB);
    glfwSetCursorPosCallback(win, cursorPosCB);
    glfwSetScrollCallback(win, scrollCB);
    glfwSetKeyCallback(win, keyCB);

    // ---- ヘッドレスモード（ネイティブ）-------------------------------------
    if (!shot.empty()) {
        if (selectArg >= 0 && selectArg < (int)app->frame.size()) {
            auto hm = buildMesh({app->frame[selectArg]}, 1.04f);
            app->hlVerts = static_cast<GLsizei>(hm.size() / 9);
            glBindBuffer(GL_ARRAY_BUFFER, app->hlVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(hm.size()*sizeof(float)),
                         hm.data(), GL_DYNAMIC_DRAW);
            auto blines = makeBBoxLines(memberBounds(app->frame[selectArg]));
            glBindBuffer(GL_ARRAY_BUFFER, app->bboxVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(blines.size()*sizeof(float)),
                         blines.data(), GL_DYNAMIC_DRAW);
            app->state.selectedMember = selectArg;
            std::printf("[select] %s\n%s",
                        app->frame[selectArg].id,
                        memberInfoConsole(app->frame[selectArg]).c_str());
        }
        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        app->draw(fbW, fbH);
        glFinish();
        if (writePPM(shot, fbW, fbH)) std::printf("wrote %s (%dx%d)\n", shot.c_str(), fbW, fbH);
        else std::fprintf(stderr, "failed to write %s\n", shot.c_str());
        glfwDestroyWindow(win);
        glfwTerminate();
        delete app;
        return 0;
    }

    // ---- メインループ -------------------------------------------------------
    app->prevTime   = glfwGetTime();
    app->frameCount = 0;

#ifdef __EMSCRIPTEN__
    // ブラウザに毎フレーム呼んでもらう（無限 while はタブを固めるため使えない）
    emscripten_set_main_loop_arg(
        [](void* p){ static_cast<App*>(p)->tick(); }, app, 0, 1);
#else
    while (!glfwWindowShouldClose(win)) app->tick();

    glfwDestroyWindow(win);
    glfwTerminate();
    delete app;
#endif
    return 0;
}
