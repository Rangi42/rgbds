// SPDX-License-Identifier: MIT

#include <ctype.h>
#include <errno.h>
#include <stack>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm/fixpoint.hpp"
#include "asm/fstack.hpp"
#include "asm/lexer.hpp"
#include "asm/section.hpp"
#include "asm/warning.hpp"

struct OptStackEntry {
	char binDigits[2];
	char gfxDigits[4];
	uint8_t fixPrecision;
	uint8_t fillByte;
	size_t maxRecursionDepth;
	DiagnosticsState<WarningID> warningStates;
};

static std::stack<OptStackEntry> stack;

void opt_B(char const chars[2]) {
	lexer_SetBinDigits(chars);
}

void opt_G(char const chars[4]) {
	lexer_SetGfxDigits(chars);
}

void opt_P(uint8_t padByte) {
	fillByte = padByte;
}

void opt_Q(uint8_t precision) {
	fixPrecision = precision;
}

void opt_R(size_t newDepth) {
	fstk_NewRecursionDepth(newDepth);
	lexer_CheckRecursionDepth();
}

void opt_W(char const *flag) {
	if (warnings.processWarningFlag(flag) == "numeric-string") {
		warning(WARNING_OBSOLETE, "Warning flag \"numeric-string\" is deprecated");
	}
}

void opt_Parse(char const *s) {
	switch (s[0]) {
	case 'b':
		if (strlen(&s[1]) == 2) {
			opt_B(&s[1]);
		} else {
			error("Must specify exactly 2 characters for option 'b'");
		}
		break;

	case 'g':
		if (strlen(&s[1]) == 4) {
			opt_G(&s[1]);
		} else {
			error("Must specify exactly 4 characters for option 'g'");
		}
		break;

	case 'p':
		if (strlen(&s[1]) <= 2) {
			int result;
			unsigned int padByte;

			result = sscanf(&s[1], "%x", &padByte);
			if (result != 1) {
				error("Invalid argument for option 'p'");
			} else if (padByte > 0xFF) {
				error("Argument for option 'p' must be between 0 and 0xFF");
			} else {
				opt_P(padByte);
			}
		} else {
			error("Invalid argument for option 'p'");
		}
		break;

		char const *precisionArg;
	case 'Q':
		precisionArg = &s[1];
		if (precisionArg[0] == '.') {
			++precisionArg;
		}
		if (strlen(precisionArg) <= 2) {
			int result;
			unsigned int precision;

			result = sscanf(precisionArg, "%u", &precision);
			if (result != 1) {
				error("Invalid argument for option 'Q'");
			} else if (precision < 1 || precision > 31) {
				error("Argument for option 'Q' must be between 1 and 31");
			} else {
				opt_Q(precision);
			}
		} else {
			error("Invalid argument for option 'Q'");
		}
		break;

	case 'r': {
		++s; // Skip 'r'
		while (isblank(*s)) {
			++s; // Skip leading whitespace
		}

		if (s[0] == '\0') {
			error("Missing argument to option 'r'");
			break;
		}

		char *endptr;
		unsigned long newDepth = strtoul(s, &endptr, 10);

		if (*endptr != '\0') {
			error("Invalid argument to option 'r' (\"%s\")", s);
		} else if (errno == ERANGE) {
			error("Argument to 'r' is out of range (\"%s\")", s);
		} else {
			opt_R(newDepth);
		}
		break;
	}

	case 'W':
		if (strlen(&s[1]) > 0) {
			opt_W(&s[1]);
		} else {
			error("Must specify an argument for option 'W'");
		}
		break;

	default:
		error("Unknown option '%c'", s[0]);
		break;
	}
}

void opt_Push() {
	OptStackEntry entry;

	// Both of these are pulled from lexer.hpp
	memcpy(entry.binDigits, binDigits, std::size(binDigits));
	memcpy(entry.gfxDigits, gfxDigits, std::size(gfxDigits));

	entry.fixPrecision = fixPrecision; // Pulled from fixpoint.hpp

	entry.fillByte = fillByte; // Pulled from section.hpp

	entry.warningStates = warnings.state; // Pulled from warning.hpp

	entry.maxRecursionDepth = maxRecursionDepth; // Pulled from fstack.h

	stack.push(entry);
}

void opt_Pop() {
	if (stack.empty()) {
		error("No entries in the option stack");
		return;
	}

	OptStackEntry entry = stack.top();
	stack.pop();

	opt_B(entry.binDigits);
	opt_G(entry.gfxDigits);
	opt_P(entry.fillByte);
	opt_Q(entry.fixPrecision);
	opt_R(entry.maxRecursionDepth);

	// `opt_W` does not apply a whole warning state; it processes one flag string
	warnings.state = entry.warningStates;
}

void opt_CheckStack() {
	if (!stack.empty()) {
		warning(WARNING_UNMATCHED_DIRECTIVE, "`PUSHO` without corresponding `POPO`");
	}
}
