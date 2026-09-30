// Minimal stand-in for Spike's riscv/trap.h (see extension.h).
#pragma once

#include <stdexcept>

class processor_t;

inline void illegal_instruction(processor_t&) { throw std::runtime_error("libgemmini: illegal instruction"); }
