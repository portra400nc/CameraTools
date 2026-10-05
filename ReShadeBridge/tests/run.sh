#!/bin/sh
# Builds and runs the bridge's tests, which play its tables against a fake ReShade runtime, with any C++17 compiler.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
for test in *_test.cpp; do
    c++ -std=c++17 -I .. "$test" -o "$out/${test%.cpp}"
    "$out/${test%.cpp}"
done
