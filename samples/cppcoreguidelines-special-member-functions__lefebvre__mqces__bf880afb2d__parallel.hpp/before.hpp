#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <utility>

#ifdef MQCES_HAVE_OPENMP
#include <omp.h>
#endif

// Tiny parallel shim. OpenMP backend when MQCES_HAVE_OPENMP is defined,
// sequential fallback otherwise.

namespace mqces::detail {

// Depth of parallel_for bodies currently executing on this thread.
inline thread_local int parallel_for_depth = 0;

// Marks the calling thread as inside a parallel_for body for its lifetime,
// including when the body exits by exception.
class ParallelForBody {
  public:
  ParallelForBody() { ++parallel_for_depth; }
  ~ParallelForBody() { --parallel_for_depth; }
  ParallelForBody(const ParallelForBody&) = delete;
  ParallelForBody& operator=(const ParallelForBody&) = delete;
};

// Call f(i) for every i in [0, n).
//
// `n_threads` > 0 caps the team size for this region only, via the
// num_threads clause; it never touches the process-wide OpenMP ICVs, so
// other OpenMP users in the process are unaffected. 0 uses the runtime
// default.
//
// A parallel_for started from inside another parallel_for's body runs
// serially on the calling thread, so the outermost loop alone decides how
// many threads are used. Without this, an outer loop capped at one thread
// would be an inactive OpenMP region and an inner region would become the
// first active level, using every core. The depth is tracked here rather
// than with omp_get_level() because MSVC implements only OpenMP 2.0.
//
// An exception may not propagate out of an OpenMP structured block (doing so
// terminates the process), so each iteration runs under try/catch. The first
// exception is kept, remaining iterations are skipped, and it is rethrown on
// the calling thread once the region has joined.
template <class F>
void parallel_for(std::size_t n, int n_threads, F&& f) {
#ifdef MQCES_HAVE_OPENMP
  if (parallel_for_depth > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      f(i);
    }
    return;
  }
  const auto n_signed = static_cast<std::ptrdiff_t>(n);
  const int team = n_threads > 0 ? n_threads : omp_get_max_threads();
  std::exception_ptr first_error;
  std::atomic<bool> failed{false};
#pragma omp parallel for schedule(static) num_threads(team)
  for (std::ptrdiff_t i = 0; i < n_signed; ++i) {
    if (failed.load(std::memory_order_relaxed)) {
      continue;
    }
    try {
      const ParallelForBody body;
      f(static_cast<std::size_t>(i));
    } catch (...) {
#pragma omp critical(mqces_parallel_for_error)
      {
        if (!first_error) {
          first_error = std::current_exception();
        }
      }
      failed.store(true, std::memory_order_relaxed);
    }
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
#else
  (void)n_threads;
  for (std::size_t i = 0; i < n; ++i) {
    f(i);
  }
#endif
}

template <class F>
void parallel_for(std::size_t n, F&& f) {
  parallel_for(n, 0, std::forward<F>(f));
}

inline int max_threads() {
#ifdef MQCES_HAVE_OPENMP
  return omp_get_max_threads();
#else
  return 1;
#endif
}

inline int current_thread_id() {
#ifdef MQCES_HAVE_OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

}  // namespace mqces::detail
