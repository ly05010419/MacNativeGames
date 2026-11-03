#pragma once
// Generators for tests/lift_{x87,int,sse}_test.cpp; see
// x87test.cpp.

#include <string>

#include "program.h"

namespace lifter {
class Decoder;
int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
int WriteIntTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
int WriteSseTest(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
}  // namespace lifter
