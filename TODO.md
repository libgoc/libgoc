# Todo

### Features

- [ ] **F1** Telemetry — expose runtime metrics for all pools, worker threads, fibers, channels, mutexes, and their metadata

- [ ] **F2** libuv I/O function wrappers that use channels rather than callbacks

- [ ] **F3** Memory-managed, mutable dynamic array
  - **F3.1** Amortized constant-time random access (read / write)
  - **F3.2** Amortized constant-time push / pop from both head and tail
  - **F3.3** Efficient concat (prepending / appending)
  - **F3.4** Efficient slicing (creating shallow-copy subarrays)

### Optimization

- [ ] **O1** Pool optimization as per benchmark results
  - [x] Fix 1: `active_count` → `_Atomic size_t`; remove `drain_mutex` from hot path in `post_to_run_queue` and `pool_worker_fn`
  - [x] Fix 3: cache last fiber stack top per worker; skip `GC_set_stackbottom` when unchanged
  - [x] Fix 4: per-pool `goc_runq_node` free-list; eliminate `malloc`/`free` on every dispatch
  - [x] Fix 5: compact fiber root list in `gc.c` (flat array + bitmap instead of linked list)
  - [ ] Fix 2: per-worker run queues with work-stealing

### Chores

- [ ] **F4** Publish to package managers for each platform (Homebrew, apt/deb, vcpkg, etc.)

- [ ] **F5** Idiomatic C++ wrapper `libgocxx`
