#pragma once
#include "scene_document.hpp"

#include <functional>

namespace vcr {
// An inverse edit against one adjacent state in the editor's linear history.
class SceneHistoryDelta {
  public:
    SceneHistoryDelta(const SceneDocument& before, const SceneDocument& after);
    [[nodiscard]] SceneDocument restore(const SceneDocument& current) const;

  private:
    std::vector<std::function<void(SceneDocument&)>> changes_;
};
} // namespace vcr
