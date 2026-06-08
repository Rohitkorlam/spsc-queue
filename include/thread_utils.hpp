#pragma once

#ifdef __linux__

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdio>
#include <string>

/// Returns the number of logical CPUs available to this process.
inline int available_cpus() noexcept {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}
inline void pin_thread(int cpu_id) {
    const int ncpus = available_cpus();
    if (cpu_id >= ncpus) {
        std::fprintf(stderr,
            "[thread_utils] WARNING: requested CPU %d but only %d available; "
            "falling back to CPU 0 (results may be noisier)\n",
            cpu_id, ncpus);
        cpu_id = 0;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    const int rc = pthread_setaffinity_np(
        pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr,
            "[thread_utils] WARNING: pthread_setaffinity_np failed for CPU %d "
            "(rc=%d); continuing unpinned\n", cpu_id, rc);
    }
}

#else  // non-Linux stub so the code still compiles

#include <cstdio>

inline void pin_thread(int cpu_id) {
    std::fprintf(stderr,
        "[thread_utils] WARNING: thread pinning not supported on this platform "
        "(requested CPU %d); continuing unpinned\n", cpu_id);
}
inline int available_cpus() noexcept { return 2; }

#endif



