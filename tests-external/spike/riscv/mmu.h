// Minimal stand-in for Spike's processor and MMU (see extension.h).
// DRAM is a sparse byte map; unwritten bytes read as 0.
#pragma once

#include "extension.h"

#include <map>
#include <utility>

class mmu_t {
public:
  std::map<uint64_t, uint8_t> mem;

  template <class T> T load(reg_t addr) {
    auto it = mem.find(addr);
    return it == mem.end() ? 0 : it->second;
  }
  template <class T> void store(reg_t addr, T val) { mem[addr] = val; }
};

struct state_t {
  struct {
    reg_t r[32] = {};
    reg_t operator[](size_t i) const { return r[i]; }
    void write(size_t i, reg_t v) { r[i] = v; }
  } XPR;
  std::map<reg_t, std::pair<reg_t, reg_t>> log_reg_write;
};

class processor_t {
public:
  mmu_t mmu;
  state_t state;
  extension_t* ext = nullptr;
  bool log_commits = false;

  mmu_t* get_mmu() { return &mmu; }
  state_t* get_state() { return &state; }
  extension_t* get_extension(const char*) { return ext; }
  bool get_log_commits_enabled() const { return log_commits; }
};
