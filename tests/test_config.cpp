// Testes do Configuration Manager. Compilar: g++ -std=c++17 -I src tests/test_config.cpp src/config.cpp -o test_config
#include "config.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using namespace vfx;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

static fs::path Write(const std::string& name, const std::string& content) {
    fs::path p = fs::temp_directory_path() / ("vfx_" + name);
    std::ofstream(p, std::ios::binary) << content;
    return p;
}
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    std::vector<std::string> w;
    Settings s;

    // 1) arquivo ausente -> padrões seguros, não lança
    CHECK(!LoadSettings("/caminho/que/nao/existe.ini", s, w));
    CHECK(s.enabled && s.profile == Profile::Balanced && s.taps == 9 && !w.empty());

    // 2) perfil balanced ignora [Sharpen] e avisa
    w.clear();
    CHECK(LoadSettings(Write("a.ini", "[General]\nProfile=balanced\n[Sharpen]\nIntensity=0.9\n"), s, w));
    CHECK(Near(s.intensity, 0.45f) && !w.empty());

    // 3) perfis aplicam presets
    w.clear();
    LoadSettings(Write("b.ini", "[General]\nProfile=performance\n"), s, w);
    CHECK(s.taps == 5 && Near(s.intensity, 0.30f) && w.empty());
    LoadSettings(Write("c.ini", "[General]\nProfile=QUALITY\n"), s, w);
    CHECK(s.profile == Profile::Quality && Near(s.intensity, 0.60f));

    // 4) custom lê valores, limita faixa, aceita vírgula decimal, comentários e CRLF
    w.clear();
    LoadSettings(Write("d.ini",
        "\xEF\xBB\xBF; comentario\r\n[General]\r\nEnabled=0 ; desligado\r\nProfile=custom\r\n"
        "[Sharpen]\r\nIntensity=1,7\r\nHaloSuppression=-0.2\r\nNoiseControl=0.25\r\nTaps=5\r\n"), s, w);
    CHECK(!s.enabled && s.profile == Profile::Custom);
    CHECK(Near(s.intensity, 1.0f) && Near(s.haloSuppression, 0.0f) && Near(s.noiseControl, 0.25f) && s.taps == 5);
    CHECK(w.size() == 2); // dois valores limitados

    // 5) lixo: valores inválidos mantêm padrão, nada quebra
    w.clear();
    LoadSettings(Write("e.ini",
        "[General]\nProfile=custom\nLogLevel=99\nEnabled=talvez\n[Sharpen]\nIntensity=abc\nTaps=7\n"
        "Intensity2=1\nlixo sem igual\n[quebrada\nNaN=nan\n"), s, w);
    CHECK(s.logLevel == 1 && s.enabled && Near(s.intensity, 0.45f) && s.taps == 9);
    CHECK(w.size() >= 5);

    // 6) NaN/Inf não passam
    w.clear();
    LoadSettings(Write("f.ini", "[General]\nProfile=custom\n[Sharpen]\nIntensity=nan\nNoiseControl=inf\n"), s, w);
    CHECK(Near(s.intensity, 0.45f) && Near(s.noiseControl, 0.40f));

    // 7) arquivo vazio -> padrões, sem erro
    w.clear();
    CHECK(LoadSettings(Write("g.ini", ""), s, w));
    CHECK(s.profile == Profile::Balanced && s.taps == 9);

    // 8) invariantes: qualquer perfil produz valores em faixa
    for (Profile p : {Profile::Performance, Profile::Balanced, Profile::Quality}) {
        Settings t; ApplyProfile(t, p);
        CHECK(t.intensity >= 0 && t.intensity <= 1 && (t.taps == 5 || t.taps == 9));
    }

    std::printf(g_fail ? "RESULTADO: %d FALHA(S)\n" : "RESULTADO: OK (todos os testes passaram)\n", g_fail);
    return g_fail ? 1 : 0;
}
