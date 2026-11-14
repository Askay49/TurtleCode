#pragma once
#include "ProjectSearchService.hpp"
#include <functional>
ProjectSearchResult scanProject(const ProjectSearchOptions& options,
    const std::shared_ptr<std::atomic_bool>& cancelled,
    const std::function<void(const ProjectSearchResult&)>& progress);
