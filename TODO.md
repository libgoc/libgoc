# Todo

### Features

- [ ] **F1** libuv I/O function wrappers that use channels rather than callbacks

- [ ] **F2** dynamic fiber stacks, like Go

- [ ] **F3** Telemetry — expose runtime metrics for all pools, worker threads, fibers, channels, mutexes, and their metadata

- [ ] **F4** Memory-managed, mutable dynamic array
  - **F4.1** Amortized constant-time random access (read / write)
  - **F4.2** Amortized constant-time push / pop from both head and tail
  - **F4.3** Efficient concat (prepending / appending)
  - **F4.4** Efficient slicing (creating shallow-copy subarrays)

### Chores

- [ ] **F4** Publish to package managers for each platform (Homebrew, apt/deb, vcpkg, etc.)

- [ ] **F5** Idiomatic C++ wrapper `libgocxx`
