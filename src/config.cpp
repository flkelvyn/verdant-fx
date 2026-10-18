#include "config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>

namespace vfx {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Remove comentário (; ou #) que comece em qualquer posição da linha.
std::string StripComment(const std::string& s) {
    size_t pos = s.find_first_of(";#");
    return pos == std::string::npos ? s : s.substr(0, pos);
}

bool ParseFloat(std::string v, float& out) {
    std::replace(v.begin(), v.end(), ',', '.');
    if (v.empty()) return false;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !std::isfinite(d)) return false;
    out = static_cast<float>(d);
    return true;
}

bool ParseInt(const std::string& v, int& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    long l = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0') return false;
    if (l < -1000000 || l > 1000000) return false;
    out = static_cast<int>(l);
    return true;
}

bool ParseBool(const std::string& v, bool& out) {
    std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on") { out = true; return true; }
    if (l == "0" || l == "false" || l == "no" || l == "off") { out = false; return true; }
    return false;
}

bool ParseProfile(const std::string& v, Profile& out) {
    std::string l = Lower(v);
    if (l == "performance") { out = Profile::Performance; return true; }
    if (l == "balanced")    { out = Profile::Balanced;    return true; }
    if (l == "quality")     { out = Profile::Quality;     return true; }
    if (l == "custom")      { out = Profile::Custom;      return true; }
    return false;
}

float Clamp01(float v, const std::string& key, std::vector<std::string>& w) {
    if (v < 0.0f || v > 1.0f) {
        w.push_back(key + ": valor " + std::to_string(v) + " fora de 0..1; limitado.");
        return std::min(1.0f, std::max(0.0f, v));
    }
    return v;
}

} // namespace

const char* ProfileName(Profile p) {
    switch (p) {
        case Profile::Performance: return "performance";
        case Profile::Balanced:    return "balanced";
        case Profile::Quality:     return "quality";
        case Profile::Custom:      return "custom";
    }
    return "balanced";
}

Settings DefaultSettings() { return Settings{}; }

void ApplyProfile(Settings& s, Profile p) {
    s.profile = p;
    switch (p) {
        case Profile::Performance:
            s.taps = 5; s.intensity = 0.30f; s.haloSuppression = 0.60f;
            s.edgePreserve = 0.50f; s.noiseControl = 0.40f; s.vegetationFocus = 0.70f;
            break;
        case Profile::Balanced:
            s.taps = 9; s.intensity = 0.45f; s.haloSuppression = 0.60f;
            s.edgePreserve = 0.50f; s.noiseControl = 0.40f; s.vegetationFocus = 0.65f;
            break;
        case Profile::Quality:
            s.taps = 9; s.intensity = 0.60f; s.haloSuppression = 0.70f;
            s.edgePreserve = 0.60f; s.noiseControl = 0.50f; s.vegetationFocus = 0.60f;
            break;
        case Profile::Custom:
            break; // mantém os valores existentes
    }
}

bool LoadSettings(const std::filesystem::path& path, Settings& out,
                  std::vector<std::string>& warnings) {
    out = DefaultSettings();

    std::ifstream f(path);
    if (!f) {
        warnings.push_back("config.ini ausente ou ilegível; usando padrões seguros.");
        return false;
    }

    std::map<std::string, std::string> kv; // "secao.chave" -> valor
    std::string line, section;
    bool first = true;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        if (first) { // BOM UTF-8
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        line = Trim(StripComment(line));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') {
                warnings.push_back("linha " + std::to_string(lineNo) + ": seção malformada; ignorada.");
                section.clear();
                continue;
            }
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warnings.push_back("linha " + std::to_string(lineNo) + ": sem '='; ignorada.");
            continue;
        }
        kv[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }

    auto take = [&](const std::string& key, std::string& val) {
        auto it = kv.find(key);
        if (it == kv.end()) return false;
        val = it->second;
        kv.erase(it);
        return true;
    };

    std::string v;
    if (take("general.enabled", v) && !ParseBool(v, out.enabled))
        warnings.push_back("General.Enabled inválido ('" + v + "'); usando 1.");
    if (take("general.loglevel", v)) {
        int n;
        if (ParseInt(v, n) && n >= 0 && n <= 2) out.logLevel = n;
        else warnings.push_back("General.LogLevel inválido ('" + v + "'); usando 1.");
    }

    Profile prof = Profile::Balanced;
    if (take("general.profile", v) && !ParseProfile(v, prof))
        warnings.push_back("General.Profile inválido ('" + v + "'); usando balanced.");

    const char* sharpenKeys[] = {"sharpen.intensity", "sharpen.halosuppression",
                                 "sharpen.edgepreservation", "sharpen.noisecontrol",
                                 "sharpen.vegetationfocus", "sharpen.taps"};
    bool anySharpenKey = false;
    for (auto* k : sharpenKeys) anySharpenKey |= kv.count(k) > 0;

    if (prof != Profile::Custom) {
        ApplyProfile(out, prof);
        if (anySharpenKey)
            warnings.push_back("Valores de [Sharpen] ignorados porque Profile != custom.");
        for (auto* k : sharpenKeys) kv.erase(k);
    } else {
        out.profile = Profile::Custom;
        auto readUnit = [&](const char* key, const char* label, float& dst) {
            if (!take(key, v)) return;
            float x;
            if (ParseFloat(v, x)) dst = Clamp01(x, label, warnings);
            else warnings.push_back(std::string(label) + " inválido ('" + v + "'); mantendo padrão.");
        };
        readUnit("sharpen.intensity", "Sharpen.Intensity", out.intensity);
        readUnit("sharpen.halosuppression", "Sharpen.HaloSuppression", out.haloSuppression);
        readUnit("sharpen.edgepreservation", "Sharpen.EdgePreservation", out.edgePreserve);
        readUnit("sharpen.noisecontrol", "Sharpen.NoiseControl", out.noiseControl);
        readUnit("sharpen.vegetationfocus", "Sharpen.VegetationFocus", out.vegetationFocus);
        if (take("sharpen.taps", v)) {
            int n;
            if (ParseInt(v, n) && (n == 5 || n == 9)) out.taps = n;
            else warnings.push_back("Sharpen.Taps inválido ('" + v + "'); use 5 ou 9. Mantendo 9.");
        }
    }

    for (auto& rest : kv)
        warnings.push_back("chave desconhecida ignorada: " + rest.first);
    return true;
}

} // namespace vfx
