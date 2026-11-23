// SPDX-License-Identifier: MIT

#include "link/fstack.hpp"

#include <stdint.h>

#include "backtrace.hpp" // printFileStackBacktrace

void FileStackNode::printBacktrace(uint32_t curLineNo) const {
	printFileStackBacktrace(this, curLineNo);
}
