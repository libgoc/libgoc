# Todo

## First Release

### Safety

- [x] **R1** `goc_alts` should assert `n_default_arms <= 1`

- [x] **R2** `goc_pool_destroy` should not be callable from within the pool being destroyed

- [x] **R3** `goc_init` and `goc_shutdown` should only be callable from the main thread
  - **R3.1** `goc_init`
  - **R3.2** `goc_shutdown`

- [x] **R4** Sync variants should assert they are not running on a fiber
  - **R4.1** `goc_take_sync`
  - **R4.2** `goc_put_sync`
  - **R4.3** `goc_alts_sync`

- [x] **R5** Go-like RW mutexes that park fibers and block threads

### Chores

- [x] **R6** Resolve circular dependencies between source files; move shared declarations to a common header and simplify the build

- [x] **R7** Build tagged artefacts for each supported platform and publish releases to GitHub

---

## Future Work

### Features

- [ ] **F1** Telemetry — expose runtime metrics for all pools, worker threads, fibers, channels, mutexes, and their metadata

- [ ] **F2** libuv I/O function wrappers that use channels rather than callbacks

- [ ] **F3** Memory-managed, mutable dynamic array
  - **F3.1** Amortized constant-time random access (read / write)
  - **F3.2** Amortized constant-time push / pop from both head and tail
  - **F3.3** Efficient concat (prepending / appending)
  - **F3.4** Efficient slicing (creating shallow-copy subarrays)

### Chores

- [ ] **F4** Publish to package managers for each platform (Homebrew, apt/deb, vcpkg, etc.)

- [ ] **F5** Idiomatic C++ wrapper `libgocxx`
