#include "renderer.hpp"
#include <algorithm>
#include <cassert>

namespace vcr {
#if VCR_EDITOR_ENABLED
void Renderer::enable_async_assets() {
    asset_requests_.reserve(1024);
    asset_preparation_.start();
    async_assets_ = true;
}
std::optional<std::uint32_t> Renderer::find_or_load_editor_model(const std::filesystem::path &root,
                                                                 std::string_view path,
                                                                 std::string &error) {
    error.clear();
    for (std::uint32_t i = 0; i < editor_model_assets_.size(); ++i) {
        const auto &model = editor_model_assets_[i];
        if (model.relative_path == path) {
            return model.ready ? std::optional(i) : std::nullopt;
        }
    }
    if (async_assets_) {
        const auto request =
            std::ranges::find_if(asset_requests_, [&](const auto &r) { return r.path == path; });
        if (request != asset_requests_.end()) {
            error = request->error;
            return {};
        }
        if (asset_requests_.size() >= 1024) {
            error = "Asset request cache reached its 1024-entry limit";
            return {};
        }
        asset_requests_.push_back({root, std::string(path)});
        ++asset_requests_pending_;
        return {};
    }
    // Initial scene loading is explicitly synchronous, outside all measured
    // frames. Each model's buffers/textures share one command submission.
    auto prepared = prepare_model_asset(root, std::string(path));
    if (!prepared.model) {
        error = prepared.error;
        return {};
    }
    asset_upload_.begin(*context_, prepared.upload_bytes + gi_model_upload_bytes(prepared));
    auto index = publish_editor_model(prepared, error);
    asset_upload_.submit();
    asset_upload_.wait();
    return index;
}
bool Renderer::poll_assets() {
    if (!async_assets_ || asset_requests_pending_ == 0) {
        return false;
    }
    bool changed = false;
    if (uploading_request_) {
        if (!asset_upload_.complete()) {
            return false;
        }
        if (uploading_model_) {
            editor_model_assets_[*uploading_model_].ready = true;
        }
        asset_requests_[*uploading_request_].done = true;
        --asset_requests_pending_;
        uploading_model_.reset();
        uploading_request_.reset();
        changed = true;
    }
    // Only this submission owner touches Vulkan. Worker results own all CPU
    // storage; stage() copies it before the result is retired at this boundary.
    if (auto completed = asset_preparation_.poll(); completed.has_value()) {
        auto &prepared = completed.value();
        const auto request = std::ranges::find_if(
            asset_requests_, [&](const auto &r) { return r.path == prepared.path; });
        assert(request != asset_requests_.end());
        if (!prepared.model) {
            request->error = std::move(prepared.error);
            request->done = true;
            --asset_requests_pending_;
            changed = true;
        } else {
            asset_upload_.begin(*context_, prepared.upload_bytes + gi_model_upload_bytes(prepared));
            uploading_model_ = publish_editor_model(prepared, request->error);
            if (uploading_model_) {
                editor_model_assets_[*uploading_model_].ready = false;
            }
            asset_upload_.submit();
            uploading_request_ = static_cast<std::size_t>(request - asset_requests_.begin());
        }
    }
    for (auto &request : asset_requests_) {
        if (!request.done && !request.queued) {
            if (!asset_preparation_.request(request.root, request.path)) {
                break;
            }
            request.queued = true;
        }
    }
    if (changed) {
        editor_scene_revision_ = 0;
        packet_document_revision_ = 0;
        shadow_dirty_ = true;
        invalidate_temporal_history();
        exposure_reset_pending_ = true;
    }
    return changed;
}
#endif
} // namespace vcr
