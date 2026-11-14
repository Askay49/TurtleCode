#pragma once
#include <QThreadPool>
namespace WorkspaceThreads {
inline QThreadPool& indexing() {static QThreadPool pool;static const bool ready=[] {pool.setMaxThreadCount(2);return true;}();(void)ready;return pool;}
inline QThreadPool& queries() {static QThreadPool pool;static const bool ready=[] {pool.setMaxThreadCount(2);return true;}();(void)ready;return pool;}
inline QThreadPool& search() {static QThreadPool pool;static const bool ready=[] {pool.setMaxThreadCount(2);return true;}();(void)ready;return pool;}
}
