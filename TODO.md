# Todo

## First Release

### Safety

- [x] **R1** `goc_alts` should assert `n_default_arms <= 1`

- [ ] **R2** `goc_pool_destroy` should not be callable from within the pool being destroyed

- [ ] **R3** `goc_init` and `goc_shutdown` should only be callable from the main thread
  - **R3.1** `goc_init`
  - **R3.2** `goc_shutdown`

- [ ] **R4** Sync variants should assert they are not running on a fiber
  - **R4.1** `goc_take_sync`
  - **R4.2** `goc_put_sync`
  - **R4.3** `goc_alts_sync`

### Chores

- [ ] **R5** Resolve circular dependencies between source files; move shared declarations to a common header and simplify the build

- [ ] **R6** Build tagged artefacts for each supported platform and publish releases to GitHub

---

## Future Work

### Features

- [ ] **F1** Fiber Local Storage (see if we can piggyback on minicoro's)

- [ ] **F2** Telemetry — expose runtime metrics for all pools, worker threads, fibers, channels, and their metadata

- [ ] **F3** libuv I/O function wrappers that use channels rather than callbacks

- [ ] **F4** Go-like RW mutexes that park fibers and block threads

- [ ] **F5** Memory-managed, mutable dynamic array
  - **F5.1** Amortized constant-time random access (read / write)
  - **F5.2** Amortized constant-time push / pop from both head and tail
  - **F5.3** Efficient concat (prepending / appending)
  - **F5.4** Efficient slicing (creating shallow-copy subarrays)

### Chores

- [ ] **F6** Publish to package managers for each platform (Homebrew, apt/deb, vcpkg, etc.)

- [ ] **F7** Idiomatic C++ wrapper `libgocxx`
