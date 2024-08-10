#!/usr/bin/env sh
FILE=target/riscv64gc-unknown-linux-gnu/$TYPE/rusty
set -e

# Rustc
cargo +nightly build --release # --color never

