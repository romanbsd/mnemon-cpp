// Thin entrypoint: all CLI wiring and command dispatch live in commands.cpp.
#include "commands.hpp"

int main(int argc, char** argv) {
  return run_mnemon(argc, argv);
}
