#include "character_ai_bridge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_http_response_bytes = 2U * 1024U * 1024U;

#if defined(_WIN32)
class ScopedWinHttpHandle final {
public:
    ScopedWinHttpHandle() noexcept = default;
    explicit ScopedWinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~ScopedWinHttpHandle() noexcept { reset(); }

    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

    ScopedWinHttpHandle(ScopedWinHttpHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    ScopedWinHttpHandle& operator=(ScopedWinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    void reset(HINTERNET next = nullptr) noexcept {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
        handle_ = next;
    }

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

[[nodiscard]] std::wstring widen_utf8(const std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

[[nodiscard]] bool winhttp_request(
    const OllamaBridgeConfig& cfg,
    const wchar_t* method,
    const wchar_t* path,
    const std::string_view body,
    std::string& out_response,
    std::string& out_error) {
    out_response.clear();
    out_error.clear();

    ScopedWinHttpHandle session(WinHttpOpen(
        L"VulkanCubeCharacterSuite/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        out_error = "WinHttpOpen failed.";
        return false;
    }
    const DWORD timeout = (std::max)(1500U, cfg.timeout_ms);
    WinHttpSetTimeouts(
        session.get(),
        2000,
        2000,
        static_cast<int>(timeout),
        static_cast<int>(timeout));

    const std::wstring whost = widen_utf8(cfg.host.empty() ? "127.0.0.1" : cfg.host);
    ScopedWinHttpHandle connect(WinHttpConnect(
        session.get(),
        whost.c_str(),
        cfg.port == 0U ? INTERNET_DEFAULT_HTTP_PORT : cfg.port,
        0));
    if (!connect) {
        out_error = "WinHttpConnect to Ollama host failed.";
        return false;
    }

    ScopedWinHttpHandle request(WinHttpOpenRequest(
        connect.get(),
        method,
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0));
    if (!request) {
        out_error = "WinHttpOpenRequest failed.";
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    const DWORD hdr_len = body.empty() ? 0UL : static_cast<DWORD>(-1L);
    const BOOL sent = WinHttpSendRequest(
        request.get(),
        body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
        hdr_len,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr)) {
        out_error = "Local Ollama server is not responding at " +
                    cfg.host + ":" + std::to_string(cfg.port) + ".";
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX) &&
        (status_code < 200 || status_code >= 300)) {
        out_error = "Ollama HTTP status " + std::to_string(status_code) + ".";
        return false;
    }

    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.get(), &available) && available > 0) {
        const std::size_t chunk = (std::min)(
            static_cast<std::size_t>(available),
            max_http_response_bytes - out_response.size());
        if (chunk == 0U) {
            out_error = "Ollama HTTP response exceeded 2 MiB limit.";
            return false;
        }
        const std::size_t old_size = out_response.size();
        out_response.resize(old_size + chunk);
        DWORD read_bytes = 0;
        if (!WinHttpReadData(
                request.get(),
                out_response.data() + old_size,
                static_cast<DWORD>(chunk),
                &read_bytes)) {
            out_error = "WinHttpReadData failed.";
            return false;
        }
        out_response.resize(old_size + read_bytes);
        if (out_response.size() >= max_http_response_bytes) {
            break;
        }
    }
    return true;
}
#endif

[[nodiscard]] std::string ascii_fold_utf8(const std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        const auto u = static_cast<unsigned char>(ch);
        if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(u - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(u));
        }
    }
    return out;
}

[[nodiscard]] bool contains_any(
    const std::string_view haystack,
    std::initializer_list<std::string_view> needles) noexcept {
    for (const std::string_view n : needles) {
        if (haystack.find(n) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] float hash_unit(const std::uint64_t seed, const std::uint32_t lane) noexcept {
    std::uint64_t z = seed + static_cast<std::uint64_t>(lane) * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31U);
    return static_cast<float>(z & 0x00FFFFFFULL) / 16777215.0F;
}

[[nodiscard]] bool call_ollama_json_prompt(
    const OllamaBridgeConfig& cfg,
    const std::string& system_and_user_prompt,
    Json& out_json,
    std::string& out_model,
    std::string& out_error) {
    if (!cfg.enable_network) {
        out_error = "Network AI disabled.";
        return false;
    }
#if defined(_WIN32)
    const OllamaStatus st = probe_ollama_status(cfg);
    if (!st.reachable || st.active_model.empty()) {
        out_error = st.diagnostic.empty()
            ? "Ollama unavailable or no models installed."
            : st.diagnostic;
        return false;
    }
    out_model = st.active_model;

    const Json req{
        {"model", st.active_model},
        {"prompt", system_and_user_prompt},
        {"format", "json"},
        {"stream", false},
        {"options", {{"temperature", 0.4}, {"num_predict", 700}}},
    };
    std::string raw_resp;
    if (!winhttp_request(cfg, L"POST", L"/api/generate", req.dump(), raw_resp, out_error)) {
        return false;
    }
    try {
        const Json outer = Json::parse(raw_resp);
        const std::string resp_text = outer.value("response", "");
        if (resp_text.empty()) {
            out_error = "Ollama returned an empty response payload.";
            return false;
        }
        out_json = Json::parse(resp_text);
        if (!out_json.is_object()) {
            out_error = "Ollama JSON payload is not an object.";
            return false;
        }
        return true;
    } catch (const Json::exception& ex) {
        out_error = std::string("Ollama returned unparseable JSON: ") + ex.what();
        return false;
    }
#else
    (void)cfg;
    (void)system_and_user_prompt;
    (void)out_json;
    (void)out_model;
    out_error = "WinHTTP client only available on Windows MVP target.";
    return false;
#endif
}

} // namespace

OllamaStatus probe_ollama_status(const OllamaBridgeConfig& config) {
    OllamaStatus status{};
    if (!config.enable_network) {
        status.diagnostic = "Ollama network disabled by config.";
        return status;
    }
#if defined(_WIN32)
    std::string raw;
    std::string err;
    if (!winhttp_request(config, L"GET", L"/api/tags", "", raw, err)) {
        status.diagnostic = err;
        return status;
    }
    try {
        const Json root = Json::parse(raw);
        if (root.contains("models") && root["models"].is_array()) {
            for (const Json& m : root["models"]) {
                const std::string name = m.value("name", "");
                if (!name.empty()) {
                    status.available_models.push_back(name);
                }
            }
        }
    } catch (const Json::exception& ex) {
        status.diagnostic = std::string("Failed parsing /api/tags: ") + ex.what();
        return status;
    }
    status.reachable = true;
    if (!config.model.empty()) {
        status.active_model = config.model;
    } else if (!status.available_models.empty()) {
        status.active_model = status.available_models.front();
    } else {
        status.diagnostic = "Ollama reachable, but no models installed (run `ollama pull llama3.2`).";
    }
#else
    status.diagnostic = "Ollama WinHTTP disabled on non-Windows host.";
#endif
    return status;
}

bool ai_generate_body_spec(
    const std::string_view prompt,
    const std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result) {
    out_result = CharacterAiGenerationResult{};
    if (!prompt.empty()) {
        in_out_package.identity.prompt = std::string(prompt);
    }
    in_out_package.identity.seed = seed;

    Json ai_json;
    std::string model_used;
    std::string err;
    const std::string full_prompt =
        "You are a 3D character anatomy designer. Respond strictly with one JSON object with keys: "
        "gender (female|male), "
        "archetype (human_balanced|human_athletic|human_heavy|human_slender|stylized_hero|veteran_wanderer), "
        "height_meters (1.45..2.25), shoulder_width (0..1), waist_width (0..1), musculature (0..1), "
        "body_fat (0..1), skin_color ([r,g,b] in 0..1), skin_roughness (0.25..0.85), jaw_width (0..1), "
        "brow_ridge (0..1), eye_color ([r,g,b]), age_wear (0..1), freckles (0..1), scar_face (bool). "
        "Prompt: " + std::string(prompt);

    if (call_ollama_json_prompt(config, full_prompt, ai_json, model_used, err)) {
        out_result.used_ollama = true;
        out_result.model_used = model_used;
        if (const auto g = parse_character_gender(ai_json.value("gender", ""))) {
            in_out_package.body.gender = *g;
        }
        if (const auto a = parse_character_archetype(ai_json.value("archetype", ""))) {
            in_out_package.body.archetype = *a;
        }
        in_out_package.body.height_meters = ai_json.value("height_meters", in_out_package.body.height_meters);
        in_out_package.body.shoulder_width = ai_json.value("shoulder_width", in_out_package.body.shoulder_width);
        in_out_package.body.waist_width = ai_json.value("waist_width", in_out_package.body.waist_width);
        in_out_package.body.musculature = ai_json.value("musculature", in_out_package.body.musculature);
        in_out_package.body.body_fat = ai_json.value("body_fat", in_out_package.body.body_fat);
        in_out_package.body.skin_roughness = ai_json.value("skin_roughness", in_out_package.body.skin_roughness);
        in_out_package.body.age_wear = ai_json.value("age_wear", in_out_package.body.age_wear);
        in_out_package.body.freckles = ai_json.value("freckles", in_out_package.body.freckles);
        in_out_package.body.face.jaw_width = ai_json.value("jaw_width", in_out_package.body.face.jaw_width);
        in_out_package.body.face.brow_ridge = ai_json.value("brow_ridge", in_out_package.body.face.brow_ridge);
        if (ai_json.contains("skin_color") && ai_json["skin_color"].is_array() && ai_json["skin_color"].size() == 3U) {
            in_out_package.body.skin_color = {
                ai_json["skin_color"][0].get<float>(),
                ai_json["skin_color"][1].get<float>(),
                ai_json["skin_color"][2].get<float>(),
            };
        }
        if (ai_json.contains("eye_color") && ai_json["eye_color"].is_array() && ai_json["eye_color"].size() == 3U) {
            in_out_package.body.face.eye_color = {
                ai_json["eye_color"][0].get<float>(),
                ai_json["eye_color"][1].get<float>(),
                ai_json["eye_color"][2].get<float>(),
            };
        }
        sanitize_character_package(in_out_package);
        out_result.summary = "Generated anatomy preset via Ollama (" + model_used + ").";
        return true;
    }

    // Deterministic offline EN/RU semantic fallback
    out_result.used_fallback = true;
    out_result.model_used = "offline_parametric_synth";
    out_result.diagnostic = err;

    const std::string low = ascii_fold_utf8(prompt);
    auto& b = in_out_package.body;
    if (contains_any(low, {
            "female", "woman", "girl", "heroine", "sister", "queen", "witch",
            "priestess", "lady", "archeress", "жен", "девуш", "героин",
            "лучниц", "воительниц", "ведьм", "жриц", "королев", "леди"})) {
        b.gender = CharacterGender::female;
    } else if (contains_any(low, {
            "male", "man", "guy", "king", "brother", "husband", "муж",
            "парен", "мужик", "корол", "брат"})) {
        b.gender = CharacterGender::male;
    }

    const bool is_female = (b.gender == CharacterGender::female);
    b.height_meters = is_female
        ? (1.64F + hash_unit(seed, 1U) * 0.10F)
        : (1.74F + hash_unit(seed, 1U) * 0.14F);
    b.shoulder_width = (is_female ? 0.38F : 0.48F) + hash_unit(seed, 2U) * 0.14F;
    b.waist_width = (is_female ? 0.34F : 0.42F) + hash_unit(seed, 3U) * 0.14F;
    b.hip_width = (is_female ? 0.56F : 0.44F) + hash_unit(seed, 8U) * 0.12F;
    b.musculature = (is_female ? 0.32F : 0.42F) + hash_unit(seed, 4U) * 0.24F;
    b.body_fat = 0.15F + hash_unit(seed, 5U) * 0.12F;
    b.age_wear = 0.08F + hash_unit(seed, 6U) * 0.18F;
    b.lip_tint = is_female ? 0.48F : 0.30F;
    in_out_package.voice.pitch_hz = is_female ? 205.0F : 122.0F;

    if (contains_any(low, {"heavy", "giant", "tank", "brute", "juggernaut", "гигант", "тяжел", "здоровяк", "танк"})) {
        b.archetype = CharacterArchetype::human_heavy;
        b.height_meters = (is_female ? 1.82F : 1.98F) + hash_unit(seed, 7U) * 0.14F;
        b.shoulder_width = is_female ? 0.70F : 0.86F;
        b.chest_depth = 0.78F;
        b.musculature = is_female ? 0.72F : 0.85F;
        b.face.jaw_width = 0.80F;
        b.face.brow_ridge = 0.78F;
    } else if (contains_any(low, {"ranger", "scout", "assassin", "slender", "elf", "рейнджер", "развед", "ловк", "строй", "охотник"})) {
        b.archetype = CharacterArchetype::human_slender;
        b.height_meters = (is_female ? 1.66F : 1.76F) + hash_unit(seed, 7U) * 0.08F;
        b.shoulder_width = is_female ? 0.36F : 0.42F;
        b.waist_width = is_female ? 0.28F : 0.34F;
        b.musculature = 0.40F;
        b.body_fat = 0.08F;
        b.face.cheekbone_prominence = 0.75F;
    } else if (contains_any(low, {"elder", "veteran", "old", "wise", "mentor", "ветеран", "стар", "мудр", "наставник"})) {
        b.archetype = CharacterArchetype::veteran_wanderer;
        b.age_wear = 0.72F;
        b.skin_roughness = 0.68F;
        b.face.brow_ridge = 0.65F;
    } else if (contains_any(low, {"soldier", "warden", "guard", "knight", "воин", "страж", "солдат", "рыцар", "охран"})) {
        b.archetype = CharacterArchetype::human_athletic;
        b.height_meters = is_female ? 1.74F : 1.84F;
        b.shoulder_width = is_female ? 0.54F : 0.68F;
        b.musculature = is_female ? 0.58F : 0.74F;
    }

    if (contains_any(low, {"dark", "ebony", "obsidian", "темн"})) {
        b.skin_color = {0.36F, 0.24F, 0.18F};
    } else if (contains_any(low, {"tan", "desert", "dune", "смугл", "пустын"})) {
        b.skin_color = {0.66F, 0.46F, 0.34F};
    } else if (contains_any(low, {"pale", "winter", "frost", "бледн", "север"})) {
        b.skin_color = {0.88F, 0.76F, 0.70F};
    }

    if (contains_any(low, {"emerald", "green", "jade", "зел", "изумруд"})) {
        b.face.eye_color = {0.16F, 0.56F, 0.30F};
    } else if (contains_any(low, {"brown", "hazel", "amber", "кар", "янтар"})) {
        b.face.eye_color = {0.42F, 0.24F, 0.12F};
    } else if (contains_any(low, {"blue", "azure", "ice", "голуб", "син"})) {
        b.face.eye_color = {0.20F, 0.46F, 0.72F};
    } else if (contains_any(low, {"gray", "grey", "silver", "сер"})) {
        b.face.eye_color = {0.48F, 0.54F, 0.58F};
    } else if (contains_any(low, {"violet", "purple", "фиолет"})) {
        b.face.eye_color = {0.44F, 0.24F, 0.62F};
    }

    if (contains_any(low, {"scar", "battle", "veteran", "шрам", "ранен"})) {
        in_out_package.paint_strokes.push_back(CharacterPaintStroke{
            .target = CharacterPaintTarget::skin,
            .mode = CharacterPaintMode::scar,
            .uv = {0.31F, 0.18F},
            .radius = 0.032F,
            .hardness = 0.85F,
            .strength = 0.90F,
            .color = {0.55F, 0.18F, 0.16F, 1.0F},
            .mirror_u = false,
        });
    }

    sanitize_character_package(in_out_package);
    out_result.summary = "Synthesized anatomy from prompt (offline fallback).";
    return true;
}

bool ai_generate_outfit_spec(
    const std::string_view prompt,
    const std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result) {
    out_result = CharacterAiGenerationResult{};

    Json ai_json;
    std::string model_used;
    std::string err;
    const std::string full_prompt =
        "You are a 3D character wardrobe and texture designer. Respond strictly with one JSON object with keys: "
        "top_style (none|tunic|jacket|trench_coat|armor_cuirass|hoodie), "
        "bottom_style (none|trousers|cargo_pants|tactical_greaves|skirt|shorts), "
        "footwear_style (barefoot|boots|combat_boots|greave_boots|sneakers), "
        "hair_style (bald|short_crop|spiky|bob|ponytail|mohawk|hood), "
        "primary_material (cloth_linen|leather|denim|tactical_weave|brushed_steel|gilded_plate), "
        "primary_color ([r,g,b]), secondary_color ([r,g,b]), accent_color ([r,g,b]), hair_color ([r,g,b]), "
        "has_belt (bool), has_pauldrons (bool), has_gloves (bool), has_cape (bool), has_beard (bool). "
        "Prompt: " + std::string(prompt);

    if (call_ollama_json_prompt(config, full_prompt, ai_json, model_used, err)) {
        out_result.used_ollama = true;
        out_result.model_used = model_used;
        auto& w = in_out_package.wardrobe;
        if (const auto ts = parse_top_garment_style(ai_json.value("top_style", ""))) w.top_style = *ts;
        if (const auto bs = parse_bottom_garment_style(ai_json.value("bottom_style", ""))) w.bottom_style = *bs;
        if (const auto fs = parse_footwear_style(ai_json.value("footwear_style", ""))) w.footwear_style = *fs;
        if (const auto hs = parse_hair_style(ai_json.value("hair_style", ""))) w.hair_style = *hs;
        if (const auto pm = parse_outfit_material_preset(ai_json.value("primary_material", ""))) w.primary_material = *pm;
        w.has_belt = ai_json.value("has_belt", w.has_belt);
        w.has_pauldrons = ai_json.value("has_pauldrons", w.has_pauldrons);
        w.has_gloves = ai_json.value("has_gloves", w.has_gloves);
        w.has_cape = ai_json.value("has_cape", w.has_cape);
        w.has_beard = ai_json.value("has_beard", w.has_beard);
        const auto read_rgb = [&](const char* key, glm::vec3& dst) {
            if (ai_json.contains(key) && ai_json[key].is_array() && ai_json[key].size() == 3U) {
                dst = {
                    ai_json[key][0].get<float>(),
                    ai_json[key][1].get<float>(),
                    ai_json[key][2].get<float>(),
                };
            }
        };
        read_rgb("primary_color", w.primary_color);
        read_rgb("secondary_color", w.secondary_color);
        read_rgb("accent_color", w.accent_color);
        read_rgb("hair_color", w.hair_color);
        sanitize_character_package(in_out_package);
        out_result.summary = "Generated wardrobe & materials via Ollama (" + model_used + ").";
        return true;
    }

    out_result.used_fallback = true;
    out_result.model_used = "offline_wardrobe_synth";
    out_result.diagnostic = err;

    const std::string low = ascii_fold_utf8(prompt);
    auto& w = in_out_package.wardrobe;
    w.has_belt = true;
    w.has_gloves = false;
    w.has_cape = false;
    w.has_pauldrons = false;

    if (contains_any(low, {"knight", "armor", "paladin", "plate", "warden", "брон", "рыцар", "лат", "страж"})) {
        w.top_style = TopGarmentStyle::armor_cuirass;
        w.bottom_style = BottomGarmentStyle::tactical_greaves;
        w.footwear_style = FootwearStyle::greave_boots;
        w.primary_material = OutfitMaterialPreset::brushed_steel;
        w.secondary_material = OutfitMaterialPreset::leather;
        w.has_pauldrons = true;
        w.has_gloves = true;
        w.primary_color = {0.52F, 0.56F, 0.62F};
        w.accent_color = {0.86F, 0.68F, 0.28F};
    } else if (contains_any(low, {"ranger", "coat", "stalker", "detective", "плащ", "сталкер", "рейнджер", "охотник"})) {
        w.top_style = TopGarmentStyle::trench_coat;
        w.bottom_style = BottomGarmentStyle::cargo_pants;
        w.footwear_style = FootwearStyle::combat_boots;
        w.primary_material = OutfitMaterialPreset::leather;
        w.hair_style = contains_any(low, {"hood", "капюш"}) ? HairStyle::hood : HairStyle::short_crop;
        w.has_gloves = true;
        w.has_cape = contains_any(low, {"cape", "cloak", "плащ"});
        w.primary_color = {0.22F, 0.25F, 0.18F};
        w.secondary_color = {0.16F, 0.15F, 0.14F};
    } else if (contains_any(low, {"hoodie", "street", "modern", "cyber", "кибер", "худи", "город"})) {
        w.top_style = TopGarmentStyle::hoodie;
        w.bottom_style = BottomGarmentStyle::cargo_pants;
        w.footwear_style = FootwearStyle::sneakers;
        w.primary_material = OutfitMaterialPreset::tactical_weave;
        w.primary_color = {0.15F, 0.30F, 0.48F};
        w.accent_color = {0.10F, 0.85F, 0.90F};
    } else {
        w.top_style = TopGarmentStyle::jacket;
        w.bottom_style = BottomGarmentStyle::trousers;
        w.footwear_style = FootwearStyle::boots;
        w.primary_material = OutfitMaterialPreset::tactical_weave;
        w.primary_color = {
            0.20F + hash_unit(seed, 10U) * 0.45F,
            0.20F + hash_unit(seed, 11U) * 0.40F,
            0.25F + hash_unit(seed, 12U) * 0.45F,
        };
    }

    if (contains_any(low, {"red", "crimson", "красн", "ал"})) {
        w.primary_color = {0.62F, 0.14F, 0.14F};
    } else if (contains_any(low, {"blue", "cobalt", "син", "голуб"})) {
        w.primary_color = {0.16F, 0.30F, 0.58F};
    } else if (contains_any(low, {"green", "emerald", "зел"})) {
        w.primary_color = {0.16F, 0.38F, 0.22F};
    } else if (contains_any(low, {"gold", "royal", "золот"})) {
        w.accent_color = {0.92F, 0.74F, 0.24F};
    }

    if (contains_any(low, {"beard", "бород"})) w.has_beard = true;
    if (contains_any(low, {"mohawk", "ирокез"})) w.hair_style = HairStyle::mohawk;
    if (contains_any(low, {"ponytail", "хвост"})) w.hair_style = HairStyle::ponytail;
    if (contains_any(low, {"bald", "лыс"})) w.hair_style = HairStyle::bald;

    sanitize_character_package(in_out_package);
    out_result.summary = "Synthesized wardrobe & textures (offline fallback).";
    return true;
}

bool ai_generate_acting_spec(
    const std::string_view prompt,
    const std::uint64_t seed,
    const OllamaBridgeConfig& config,
    CharacterPackage& in_out_package,
    CharacterAiGenerationResult& out_result) {
    out_result = CharacterAiGenerationResult{};

    Json ai_json;
    std::string model_used;
    std::string err;
    const std::string full_prompt =
        "You are a game narrative and character acting designer. Respond strictly with one JSON object for character '" +
        in_out_package.identity.display_name + "' (" + std::string(to_token(in_out_package.identity.role)) +
        "): keys temperament (neutral|warm|stern|cheerful|guarded|heroic), pitch_hz (85..280), speech_rate (0.7..1.4), "
        "nodes (array of up to 3 objects: {id, text, text_ru, expression_clip (talk|wave|idle|attack), "
        "choices: [{text, text_ru, next_node_id, close}]}). Context: " + std::string(prompt);

    if (call_ollama_json_prompt(config, full_prompt, ai_json, model_used, err)) {
        out_result.used_ollama = true;
        out_result.model_used = model_used;
        if (const auto t = parse_character_temperament(ai_json.value("temperament", ""))) {
            in_out_package.acting.temperament = *t;
        }
        in_out_package.voice.pitch_hz = ai_json.value("pitch_hz", in_out_package.voice.pitch_hz);
        in_out_package.voice.speech_rate = ai_json.value("speech_rate", in_out_package.voice.speech_rate);
        if (ai_json.contains("nodes") && ai_json["nodes"].is_array() && !ai_json["nodes"].empty()) {
            in_out_package.dialogue.nodes.clear();
            for (const Json& jn : ai_json["nodes"]) {
                if (!jn.is_object()) continue;
                DialogueNodeSpec dn{};
                dn.id = jn.value("id", "node_" + std::to_string(in_out_package.dialogue.nodes.size()));
                dn.speaker = in_out_package.identity.display_name;
                dn.text = jn.value("text", "Greetings.");
                dn.text_ru = jn.value("text_ru", dn.text);
                dn.voice_clip_id = dn.id;
                dn.expression_clip = jn.value("expression_clip", "talk");
                if (jn.contains("choices") && jn["choices"].is_array()) {
                    for (const Json& jc : jn["choices"]) {
                        if (!jc.is_object()) continue;
                        DialogueChoiceSpec ch{};
                        ch.text = jc.value("text", "Continue");
                        ch.text_ru = jc.value("text_ru", ch.text);
                        ch.next_node_id = jc.value("next_node_id", "");
                        ch.closes_dialogue = jc.value("close", ch.next_node_id.empty());
                        dn.choices.push_back(std::move(ch));
                    }
                }
                in_out_package.dialogue.nodes.push_back(std::move(dn));
            }
            if (!in_out_package.dialogue.nodes.empty()) {
                in_out_package.dialogue.entry_node_id = in_out_package.dialogue.nodes.front().id;
            }
        }
        sanitize_character_package(in_out_package);
        out_result.summary = "Generated voice, acting & dialogue via Ollama (" + model_used + ").";
        return true;
    }

    out_result.used_fallback = true;
    out_result.model_used = "offline_dialogue_synth";
    out_result.diagnostic = err;

    const std::string low = ascii_fold_utf8(prompt);
    const std::string& name = in_out_package.identity.display_name;
    auto& a = in_out_package.acting;
    auto& v = in_out_package.voice;

    const bool is_female = (in_out_package.body.gender == CharacterGender::female);
    v.pitch_hz = is_female
        ? (195.0F + hash_unit(seed, 20U) * 45.0F)
        : (105.0F + hash_unit(seed, 20U) * 45.0F);
    if (contains_any(low, {"vendor", "merchant", "shop", "торгов", "куп"})) {
        a.temperament = CharacterTemperament::cheerful;
        a.default_smile = 0.45F;
        in_out_package.dialogue.entry_node_id = "shop_welcome";
        in_out_package.dialogue.nodes = {
            DialogueNodeSpec{
                .id = "shop_welcome",
                .speaker = name,
                .text = "Finest supplies on this side of the frontier. What catches your eye?",
                .text_ru = "Лучшее снаряжение по эту сторону рубежа. Что тебя интересует?",
                .voice_clip_id = "shop_welcome",
                .expression_clip = "wave",
                .smile = 0.50F,
                .brow_raise = 0.25F,
                .choices = {
                    {"Tell me about rare gear.", "Расскажи о редких товарах.", "shop_rare", "asked_rare", 1.0F, false},
                    {"Just looking around.", "Просто смотрю.", "", "", 0.0F, true},
                },
            },
            DialogueNodeSpec{
                .id = "shop_rare",
                .speaker = name,
                .text = "Fresh optical scopes arrived before sunrise. Keep your credits ready.",
                .text_ru = "Свежие прицелы прибыли на рассвете. Готовь кредиты.",
                .voice_clip_id = "shop_rare",
                .expression_clip = "talk",
                .smile = 0.30F,
                .choices = {
                    {"I will come back later.", "Я вернусь позже.", "", "", 0.0F, true},
                },
            },
        };
    } else if (contains_any(low, {"stern", "boss", "commander", "guard", "суров", "команд", "босс", "охран"})) {
        a.temperament = CharacterTemperament::stern;
        a.default_frown = 0.35F;
        v.pitch_hz = is_female ? 168.0F : 96.0F;
        in_out_package.dialogue.entry_node_id = "challenge";
        in_out_package.dialogue.nodes = {
            DialogueNodeSpec{
                .id = "challenge",
                .speaker = name,
                .text = "State your clearance code before taking another step into this sector.",
                .text_ru = "Назови код допуска, прежде чем сделать ещё шаг в этот сектор.",
                .voice_clip_id = "challenge",
                .expression_clip = "talk",
                .brow_raise = 0.10F,
                .frown = 0.42F,
                .choices = {
                    {"Relay verification code Omega-Seven.", "Код подтверждения Омега-Семь.", "cleared", "clearance_ok", 1.0F, false},
                    {"Step aside, I have no time.", "Отойди, у меня нет времени.", "", "hostile_warning", 1.0F, true},
                },
            },
            DialogueNodeSpec{
                .id = "cleared",
                .speaker = name,
                .text = "Authorization confirmed. Watch the northern catwalks.",
                .text_ru = "Допуск подтверждён. Следи за северными переходами.",
                .voice_clip_id = "cleared",
                .expression_clip = "idle",
                .choices = {
                    {"Copy that.", "Принято.", "", "", 0.0F, true},
                },
            },
        };
    } else {
        a.temperament = CharacterTemperament::heroic;
        a.default_smile = 0.15F;
        in_out_package.dialogue.entry_node_id = "greeting";
        in_out_package.dialogue.nodes = make_default_character_package(
            in_out_package.identity.slug, name).dialogue.nodes;
    }

    sanitize_character_package(in_out_package);
    out_result.summary = "Synthesized voice & dialogue graph (offline fallback).";
    return true;
}

} // namespace vcr
