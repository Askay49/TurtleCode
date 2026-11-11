#pragma once
#include "GitService.hpp"

// Each call owns its libgit2 handles and runs on a worker thread.
namespace GitBackend {
QString discover(const QString& root);
GitStatus status(const QString& repository);
GitDiffSnapshot versions(GitDiffSnapshot snapshot);
}
