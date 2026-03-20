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

### Chores

- [ ] **F4** Publish to package managers for each platform (Homebrew, apt/deb, vcpkg, etc.)

- [ ] **F5** Idiomatic C++ wrapper `libgocxx`
