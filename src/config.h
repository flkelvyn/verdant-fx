// Verdant FX — Configuration Manager (portável: sem dependências do Windows)
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace vfx {

enum class Profile { Performance, Balanced, Quality, Custom };

struct Settings {
    bool    enabled         = true;
    Profile profile         = Profile::Balanced;
    int     logLevel        = 1;      // 0 = erros, 1 = info, 2 = debug
    float   intensity       = 0.45f;  // 0..1
    float   haloSuppression = 0.60f;  // 0..1
    float   edgePreserve    = 0.50f;  // 0..1
    float   noiseControl    = 0.40f;  // 0..1
    float   vegetationFocus = 0.65f;  // 0..1 (0 = igual em toda a imagem)
    int     taps            = 9;      // 5 ou 9
};

// Valores padrão seguros (usados se o arquivo faltar ou estiver corrompido).
Settings DefaultSettings();

// Aplica os valores do perfil (exceto Custom, que mantém os valores do arquivo).
void ApplyProfile(Settings& s, Profile p);

// Lê e valida o arquivo. Nunca lança exceção e nunca deixa 'out' inválido:
//  - arquivo ausente/ilegível  -> padrões seguros, retorna false;
//  - valor inválido/fora da faixa -> padrão ou limite, com aviso em 'warnings'.
// Retorna true somente se o arquivo foi lido.
bool LoadSettings(const std::filesystem::path& path, Settings& out,
                  std::vector<std::string>& warnings);

const char* ProfileName(Profile p);

} // namespace vfx
