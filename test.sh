#! /bin/bash
rm -rf build/ \
    && cmake -S . -B build \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure
