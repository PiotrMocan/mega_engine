#include "localization.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace vcr {
namespace {

struct BuiltinLocPair {
    std::string_view key;
    std::string_view en;
    std::string_view ru;
};

constexpr auto kBuiltinTranslations = std::to_array<BuiltinLocPair>({
    {"ui.menu.title", "MAIN MENU", "ГЛАВНОЕ МЕНЮ"},
    {"ui.menu.subtitle", "REALTIME GAMEPLAY RUNTIME", "ИГРОВАЯ СРЕДА РЕАЛЬНОГО ВРЕМЕНИ"},
    {"ui.menu.continue", "Continue", "Продолжить"},
    {"ui.menu.new_game", "New Game", "Новая игра"},
    {"ui.menu.play", "Play", "Играть"},
    {"ui.menu.settings", "Settings", "Настройки"},
    {"ui.menu.quit", "Quit", "Выход"},
    {"ui.pause.title", "PAUSED", "ПАУЗА"},
    {"ui.pause.resume", "Resume", "Продолжить"},
    {"ui.pause.restart", "Restart", "Начать заново"},
    {"ui.pause.settings", "Settings", "Настройки"},
    {"ui.pause.main_menu", "Main Menu", "Главное меню"},
    {"ui.pause.quit", "Quit Game", "Выйти из игры"},
    {"ui.settings.title", "SETTINGS", "НАСТРОЙКИ"},
    {"ui.settings.tab.graphics", "Graphics", "Графика"},
    {"ui.settings.tab.audio", "Audio", "Звук"},
    {"ui.settings.tab.controls", "Controls", "Управление"},
    {"ui.settings.tab.interface", "Interface", "Интерфейс"},
    {"ui.settings.present_mode", "Present Mode", "Режим вывода"},
    {"ui.settings.fullscreen", "Fullscreen", "Полный экран"},
    {"ui.settings.anti_aliasing", "Anti-Aliasing", "Сглаживание"},
    {"ui.settings.shadows", "Shadows", "Тени"},
    {"ui.settings.shadow_res", "Shadow Quality", "Качество теней"},
    {"ui.settings.sky", "Sky", "Небо"},
    {"ui.settings.exposure", "Exposure", "Экспозиция"},
    {"ui.settings.bloom", "Bloom", "Свечение"},
    {"ui.settings.master_volume", "Master Volume", "Общая громкость"},
    {"ui.settings.sfx_volume", "Effects Volume", "Эффекты"},
    {"ui.settings.ambience_volume", "Voice / Ambience", "Голос / Окружение"},
    {"ui.settings.voice_volume", "Voice / Ambience", "Голос / Окружение"},
    {"ui.settings.music_volume", "Music Volume", "Музыка"},
    {"ui.settings.ui_volume", "UI Volume", "Интерфейс"},
    {"ui.settings.mute_all", "Mute Audio", "Без звука"},
    {"ui.settings.look_sensitivity", "Camera Sensitivity", "Чувствительность камеры"},
    {"ui.settings.gamepad_deadzone", "Gamepad Deadzone", "Мертвая зона стика"},
    {"ui.settings.invert_y", "Invert Look Y", "Инверсия оси Y"},
    {"ui.settings.reset_bindings", "Reset Default Bindings", "Сбросить управление"},
    {"ui.settings.press_key_prompt", "Press key or gamepad button (Esc cancels)...", "Нажмите клавишу или кнопку геймпада (Esc - отмена)..."},
    {"ui.settings.language", "Language", "Язык"},
    {"ui.settings.scale_mode", "UI Scale Mode", "Масштабирование UI"},
    {"ui.settings.ui_scale", "UI Scale", "Масштаб интерфейса"},
    {"ui.settings.safe_area", "Safe Area Margin", "Отступ от краев"},
    {"ui.settings.show_hud", "Show HUD", "Показывать HUD"},
    {"ui.settings.show_crosshair", "Show Crosshair", "Показывать прицел"},
    {"ui.settings.show_subtitles", "Show Subtitles", "Показывать субтитры"},
    {"ui.settings.show_markers", "Show World Markers", "Маркеры в мире"},
    {"ui.settings.back", "Back", "Назад"},
    {"ui.scale_mode.fit", "Fit 1080p", "Адаптивный 1080p"},
    {"ui.scale_mode.match_height", "Match Height", "По высоте"},
    {"ui.scale_mode.match_width", "Match Width", "По ширине"},
    {"ui.scale_mode.pixel", "Pixel 1:1", "Пиксельный 1:1"},
    {"ui.common.on", "On", "Вкл"},
    {"ui.common.off", "Off", "Выкл"},
    {"ui.common.yes", "Yes", "Да"},
    {"ui.common.no", "No", "Нет"},
    {"ui.common.default", "Default", "По умолчанию"},
    {"ui.common.unbound", "Unbound", "Не назначено"},
    {"ui.common.health", "HEALTH", "ЗДОРОВЬЕ"},
    {"ui.common.interact", "Interact", "Взаимодействие"},
});

constexpr auto kBuiltinTranslationsExtra = std::to_array<BuiltinLocPair>({
    {"ui.common.fps", "FPS", "КАДР/С"},
    {"ui.common.frame_ms", "MS", "МС"},
    {"ui.common.meters_short", "m", "м"},
    {"ui.common.speaker", "Speaker", "Собеседник"},
    {"ui.hint.menu_nav", "W/S or Arrows: Navigate  |  A/D: Adjust  |  Enter: Select  |  Esc: Back", "W/S или Стрелки: Выбор  |  A/D: Изменить  |  Enter: Принять  |  Esc: Назад"},
    {"ui.hint.dialogue_nav", "Up/Down: Choice  |  Enter / E: Confirm  |  Esc: Close", "Вверх/Вниз: Выбор  |  Enter / E: Ответить  |  Esc: Закрыть"},
    {"ui.hint.interact_key", "[{key}] {action}", "[{key}] {action}"},
    {"ui.action.move_forward", "Move Forward", "Вперед"},
    {"ui.action.move_backward", "Move Backward", "Назад"},
    {"ui.action.move_left", "Strafe Left", "Влево"},
    {"ui.action.move_right", "Strafe Right", "Вправо"},
    {"ui.action.move_up", "Move Up", "Вверх"},
    {"ui.action.move_down", "Move Down", "Вниз"},
    {"ui.action.sprint", "Sprint / Boost", "Ускорение"},
    {"ui.action.interact", "Interact", "Взаимодействие"},
    {"ui.action.pause", "Pause / Menu", "Пауза / Меню"},
    {"ui.action.quick_reset", "Quick Reset", "Быстрый сброс"},
    {"ui.action.recover", "Recover", "Возврат"},
});

void upsert_sorted(std::vector<LocalizationEntry>& entries, std::string_view key, std::string_view value)
{
    auto it = std::lower_bound(
        entries.begin(),
        entries.end(),
        key,
        [](const LocalizationEntry& lhs, const std::string_view rhs) { return lhs.key < rhs; });
    if (it != entries.end() && it->key == key) {
        it->value.assign(value);
        return;
    }
    entries.insert(it, LocalizationEntry{std::string(key), std::string(value)});
}

void append_text(std::span<char> buffer, std::size_t& cursor, std::string_view text) noexcept
{
    if (cursor + 1U >= buffer.size()) {
        return;
    }
    const std::size_t available = buffer.size() - 1U - cursor;
    const std::size_t copy_len = std::min(available, text.size());
    for (std::size_t i = 0U; i < copy_len; ++i) {
        buffer[cursor + i] = text[i];
    }
    cursor += copy_len;
}

} // namespace

LocalizationSystem::LocalizationSystem()
{
    reset_to_builtins();
}

void LocalizationSystem::reset_to_builtins()
{
    tables_.clear();
    const std::size_t en_idx = ensure_table("en", "English");
    const std::size_t ru_idx = ensure_table("ru", "Русский");

    for (const auto& pair : kBuiltinTranslations) {
        upsert_sorted(tables_[en_idx].entries, pair.key, pair.en);
        upsert_sorted(tables_[ru_idx].entries, pair.key, pair.ru);
    }
    for (const auto& pair : kBuiltinTranslationsExtra) {
        upsert_sorted(tables_[en_idx].entries, pair.key, pair.en);
        upsert_sorted(tables_[ru_idx].entries, pair.key, pair.ru);
    }
    tables_[en_idx].sorted = true;
    tables_[ru_idx].sorted = true;

    active_locale_ = "en";
    fallback_locale_ = "en";
    refresh_indices();
}

std::string LocalizationSystem::normalize_locale_code(std::string_view locale)
{
    std::string out;
    out.reserve(locale.size());
    for (const unsigned char ch : locale) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch == '_') {
            out.push_back('-');
        } else {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return out;
}

std::size_t LocalizationSystem::find_table_index(std::string_view normalized_locale) const noexcept
{
    for (std::size_t i = 0U; i < tables_.size(); ++i) {
        if (tables_[i].locale == normalized_locale) {
            return i;
        }
    }
    return tables_.size();
}

std::size_t LocalizationSystem::ensure_table(std::string_view normalized_locale, std::string_view display_name)
{
    const std::size_t existing = find_table_index(normalized_locale);
    if (existing < tables_.size()) {
        if (!display_name.empty()) {
            tables_[existing].display_name.assign(display_name);
        }
        return existing;
    }
    LocaleTable table{};
    table.locale.assign(normalized_locale);
    table.display_name = display_name.empty() ? std::string(normalized_locale) : std::string(display_name);
    tables_.push_back(std::move(table));
    refresh_indices();
    return tables_.size() - 1U;
}

void LocalizationSystem::refresh_indices() noexcept
{
    fallback_table_index_ = find_table_index(fallback_locale_);
    active_table_index_ = find_table_index(active_locale_);
    primary_fallback_table_index_ = tables_.size();

    const std::size_t dash = active_locale_.find('-');
    if (dash != std::string::npos && dash > 0U) {
        primary_fallback_table_index_ = find_table_index(std::string_view(active_locale_).substr(0U, dash));
    }
    if (active_table_index_ >= tables_.size() && primary_fallback_table_index_ < tables_.size()) {
        active_table_index_ = primary_fallback_table_index_;
    }
}

bool LocalizationSystem::set_locale(std::string_view locale)
{
    const std::string normalized = normalize_locale_code(locale);
    if (normalized.empty()) {
        return false;
    }
    const std::size_t exact = find_table_index(normalized);
    std::size_t primary = tables_.size();
    const std::size_t dash = normalized.find('-');
    if (dash != std::string::npos && dash > 0U) {
        primary = find_table_index(std::string_view(normalized).substr(0U, dash));
    }
    if (exact >= tables_.size() && primary >= tables_.size()) {
        return false;
    }
    active_locale_ = normalized;
    refresh_indices();
    return true;
}

bool LocalizationSystem::set_fallback_locale(std::string_view locale)
{
    const std::string normalized = normalize_locale_code(locale);
    if (normalized.empty() || find_table_index(normalized) >= tables_.size()) {
        return false;
    }
    fallback_locale_ = normalized;
    refresh_indices();
    return true;
}

std::string_view LocalizationSystem::display_name(std::string_view locale) const noexcept
{
    // Linear small scan avoiding allocations for common short normalized ASCII codes
    for (const auto& table : tables_) {
        if (table.locale.size() != locale.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0U; i < locale.size(); ++i) {
            const char a = table.locale[i];
            const unsigned char raw_b = static_cast<unsigned char>(locale[i]);
            const char b = (raw_b == '_') ? '-' : static_cast<char>(std::tolower(raw_b));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return table.display_name;
        }
    }
    return {};
}

std::vector<LocalizationLocaleInfo> LocalizationSystem::available_locales() const
{
    std::vector<LocalizationLocaleInfo> result;
    result.reserve(tables_.size());
    for (const auto& table : tables_) {
        result.push_back(LocalizationLocaleInfo{
            .locale = table.locale,
            .display_name = table.display_name,
            .entry_count = table.entries.size(),
        });
    }
    return result;
}

void LocalizationSystem::set_entry(
    std::string_view locale,
    std::string_view key,
    std::string_view value,
    std::string_view display_name)
{
    if (key.empty()) {
        return;
    }
    const std::string normalized = normalize_locale_code(locale);
    if (normalized.empty()) {
        return;
    }
    const std::size_t idx = ensure_table(normalized, display_name);
    upsert_sorted(tables_[idx].entries, key, value);
    refresh_indices();
}

std::string_view LocalizationSystem::find_in_table(const LocaleTable& table, std::string_view key) noexcept
{
    const auto it = std::lower_bound(
        table.entries.begin(),
        table.entries.end(),
        key,
        [](const LocalizationEntry& lhs, const std::string_view rhs) { return lhs.key < rhs; });
    if (it != table.entries.end() && it->key == key) {
        return it->value;
    }
    return {};
}

bool LocalizationSystem::contains(std::string_view key) const noexcept
{
    return !try_get(key).empty() || (
        active_table_index_ < tables_.size() &&
        !find_in_table(tables_[active_table_index_], key).empty());
}

std::string_view LocalizationSystem::try_get(std::string_view key) const noexcept
{
    if (key.empty()) {
        return {};
    }
    if (active_table_index_ < tables_.size()) {
        const std::string_view v = find_in_table(tables_[active_table_index_], key);
        if (!v.empty()) {
            return v;
        }
    }
    if (primary_fallback_table_index_ < tables_.size() && primary_fallback_table_index_ != active_table_index_) {
        const std::string_view v = find_in_table(tables_[primary_fallback_table_index_], key);
        if (!v.empty()) {
            return v;
        }
    }
    if (fallback_table_index_ < tables_.size() && fallback_table_index_ != active_table_index_) {
        const std::string_view v = find_in_table(tables_[fallback_table_index_], key);
        if (!v.empty()) {
            return v;
        }
    }
    return {};
}

std::string_view LocalizationSystem::get(std::string_view key) const noexcept
{
    const std::string_view found = try_get(key);
    return found.empty() ? key : found;
}

std::string_view LocalizationSystem::resolve(std::string_view key_or_text) const noexcept
{
    if (key_or_text.empty()) {
        return {};
    }
    if (key_or_text.front() == '@') {
        const std::string_view key = key_or_text.substr(1U);
        return get(key);
    }
    const std::string_view found = try_get(key_or_text);
    return found.empty() ? key_or_text : found;
}

std::string_view LocalizationSystem::format_into(
    std::span<char> buffer,
    std::string_view key_or_text,
    std::span<const LocalizationArg> args) const noexcept
{
    if (buffer.empty()) {
        return {};
    }
    const std::string_view tmpl = resolve(key_or_text);
    std::size_t cursor = 0U;

    std::size_t pos = 0U;
    while (pos < tmpl.size() && cursor + 1U < buffer.size()) {
        const std::size_t open = tmpl.find('{', pos);
        if (open == std::string_view::npos) {
            append_text(buffer, cursor, tmpl.substr(pos));
            break;
        }
        if (open > pos) {
            append_text(buffer, cursor, tmpl.substr(pos, open - pos));
        }
        const std::size_t close = tmpl.find('}', open + 1U);
        if (close == std::string_view::npos) {
            append_text(buffer, cursor, tmpl.substr(open));
            break;
        }
        const std::string_view name = tmpl.substr(open + 1U, close - open - 1U);
        bool substituted = false;
        for (const auto& arg : args) {
            if (arg.name == name) {
                append_text(buffer, cursor, arg.value);
                substituted = true;
                break;
            }
        }
        if (!substituted) {
            append_text(buffer, cursor, tmpl.substr(open, close - open + 1U));
        }
        pos = close + 1U;
    }

    buffer[cursor] = '\0';
    return std::string_view(buffer.data(), cursor);
}

bool LocalizationSystem::load_file(const std::filesystem::path& path, std::string* error_message)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open localization file: " + path.string();
        }
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        if (error_message != nullptr) {
            *error_message = "Failed to parse localization JSON '" + path.string() + "': " + e.what();
        }
        return false;
    }

    if (!root.is_object()) {
        if (error_message != nullptr) {
            *error_message = "Localization file root must be a JSON object: " + path.string();
        }
        return false;
    }

    auto load_entries_map = [&](std::string_view loc, std::string_view disp, const nlohmann::json& entries_obj) {
        const std::string normalized = normalize_locale_code(loc);
        if (normalized.empty() || !entries_obj.is_object()) {
            return;
        }
        const std::size_t table_idx = ensure_table(normalized, disp);
        for (const auto& [k, v] : entries_obj.items()) {
            if (v.is_string()) {
                upsert_sorted(tables_[table_idx].entries, k, v.get_ref<const std::string&>());
            }
        }
        tables_[table_idx].sorted = true;
    };

    if (root.contains("locales") && root["locales"].is_object()) {
        for (const auto& [loc_key, loc_val] : root["locales"].items()) {
            if (!loc_val.is_object()) {
                continue;
            }
            const std::string disp = loc_val.value("display_name", std::string{});
            if (loc_val.contains("entries") && loc_val["entries"].is_object()) {
                load_entries_map(loc_key, disp, loc_val["entries"]);
            } else {
                load_entries_map(loc_key, disp, loc_val);
            }
        }
        refresh_indices();
        return true;
    }

    std::string locale_code = root.value("locale", std::string{});
    if (locale_code.empty()) {
        const std::string stem = path.stem().string();
        const std::string suffix = ".vcrloc";
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            locale_code = stem.substr(0U, stem.size() - suffix.size());
        } else {
            locale_code = stem;
        }
    }
    const std::string disp = root.value("display_name", std::string{});
    if (!root.contains("entries") || !root["entries"].is_object()) {
        if (error_message != nullptr) {
            *error_message = "Localization file missing 'entries' object: " + path.string();
        }
        return false;
    }
    load_entries_map(locale_code, disp, root["entries"]);
    refresh_indices();
    return true;
}

std::size_t LocalizationSystem::load_directory(const std::filesystem::path& directory, std::string* error_message)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
        return 0U;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (filename.ends_with(".vcrloc.json") || entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::size_t loaded = 0U;
    for (const auto& file : files) {
        if (load_file(file, error_message)) {
            ++loaded;
        }
    }
    return loaded;
}

bool LocalizationSystem::save_file(
    const std::filesystem::path& path,
    std::string_view locale,
    std::string* error_message) const
{
    const std::string normalized = normalize_locale_code(locale);
    const std::size_t idx = find_table_index(normalized);
    if (idx >= tables_.size()) {
        if (error_message != nullptr) {
            *error_message = "Unknown locale '" + std::string(locale) + "'";
        }
        return false;
    }

    const LocaleTable& table = tables_[idx];
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& entry : table.entries) {
        entries[entry.key] = entry.value;
    }
    const nlohmann::json root{
        {"format", "vcr_localization_v1"},
        {"locale", table.locale},
        {"display_name", table.display_name},
        {"entries", std::move(entries)},
    };

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open localization output file: " + path.string();
        }
        return false;
    }
    out << root.dump(2) << '\n';
    if (!out.good()) {
        if (error_message != nullptr) {
            *error_message = "Failed while writing localization file: " + path.string();
        }
        return false;
    }
    return true;
}

} // namespace vcr
