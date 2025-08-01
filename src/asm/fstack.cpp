// SPDX-License-Identifier: MIT

#include "asm/fstack.hpp"
#include <sys/stat.h>

#include <deque>
#include <errno.h>
#include <inttypes.h>
#include <memory>
#include <stack>
#include <stdio.h>
#include <stdlib.h>

#include "diagnostics.hpp"
#include "helpers.hpp"
#include "linkdefs.hpp"
#include "platform.hpp" // S_ISDIR (stat macro)

#include "asm/lexer.hpp"
#include "asm/macro.hpp"
#include "asm/main.hpp"
#include "asm/symbol.hpp"
#include "asm/warning.hpp"

using namespace std::literals;

struct Context {
	std::shared_ptr<FileStackNode> fileInfo;
	LexerState lexerState{};
	// If the shared_ptr is empty, `\@` is not permitted for this context.
	// Otherwise, if the pointee string is empty, it means that a unique ID has not been requested
	// for this context yet, and it should be generated.
	// Note that several contexts can share the same unique ID (since `INCLUDE` preserves its
	// parent's, and likewise "back-propagates" a unique ID if requested), hence using `shared_ptr`.
	std::shared_ptr<std::string> uniqueIDStr = nullptr;
	std::shared_ptr<MacroArgs> macroArgs = nullptr; // Macro args are *saved* here
	uint32_t nbReptIters = 0;
	bool isForLoop = false;
	int32_t forValue = 0;
	int32_t forStep = 0;
	std::string forName{};
};

static std::stack<Context> contextStack;

// The first include path for `fstk_FindFile` to try is none at all
static std::vector<std::string> includePaths = {""}; // -I
static std::deque<std::string> preIncludeNames;      // -P
static bool failedOnMissingInclude = false;

std::string FileStackNode::reptChain() const {
	std::string chain;
	std::vector<uint32_t> const &nodeIters = iters();
	for (uint32_t i = nodeIters.size(); i--;) {
		chain.append("::REPT~");
		chain.append(std::to_string(nodeIters[i]));
	}
	return chain;
}

std::string const &FileStackNode::dump(uint32_t curLineNo) const {
	if (std::holds_alternative<std::vector<uint32_t>>(data)) {
		assume(parent); // REPT nodes use their parent's name
		std::string const &lastName = parent->dump(lineNo);
		fputs(" -> ", stderr);
		fputs(lastName.c_str(), stderr);
		fputs(reptChain().c_str(), stderr);
		fprintf(stderr, "(%" PRIu32 ")", curLineNo);
		return lastName;
	} else {
		if (parent) {
			parent->dump(lineNo);
			fputs(" -> ", stderr);
		}
		std::string const &nodeName = name();
		fputs(nodeName.c_str(), stderr);
		fprintf(stderr, "(%" PRIu32 ")", curLineNo);
		return nodeName;
	}
}

bool fstk_DumpCurrent() {
	if (lexer_AtTopLevel()) {
		return false;
	}
	assume(!contextStack.empty());
	contextStack.top().fileInfo->dump(lexer_GetLineNo());
	return true;
}

std::shared_ptr<FileStackNode> fstk_GetFileStack() {
	return contextStack.empty() ? nullptr : contextStack.top().fileInfo;
}

std::shared_ptr<std::string> fstk_GetUniqueIDStr() {
	static uint64_t nextUniqueID = 1;

	std::shared_ptr<std::string> &str = contextStack.top().uniqueIDStr;

	// If a unique ID is allowed but has not been generated yet, generate one now.
	if (str && str->empty()) {
		*str = "_u"s + std::to_string(nextUniqueID++);
	}

	return str;
}

MacroArgs *fstk_GetCurrentMacroArgs() {
	// This returns a raw pointer, *not* a shared pointer, so its returned value
	// does *not* keep the current macro args alive!
	return contextStack.top().macroArgs.get();
}

void fstk_AddIncludePath(std::string const &path) {
	if (path.empty()) {
		return;
	}

	std::string &includePath = includePaths.emplace_back(path);
	if (includePath.back() != '/') {
		includePath += '/';
	}
}

void fstk_AddPreIncludeFile(std::string const &path) {
	preIncludeNames.emplace_front(path);
	verbosePrint("Pre-included filename %s\n", path.c_str()); // LCOV_EXCL_LINE
}

static bool isValidFilePath(std::string const &path) {
	struct stat statBuf;
	return stat(path.c_str(), &statBuf) == 0 && !S_ISDIR(statBuf.st_mode); // Reject directories
}

static void printDep(std::string const &path) {
	options.printDep(path);
	if (options.dependFile && options.generatePhonyDeps && isValidFilePath(path)) {
		fprintf(options.dependFile, "%s:\n", path.c_str());
	}
}

std::optional<std::string> fstk_FindFile(std::string const &path) {
	for (std::string &incPath : includePaths) {
		if (std::string fullPath = incPath + path; isValidFilePath(fullPath)) {
			printDep(fullPath);
			return fullPath;
		}
	}

	errno = ENOENT;
	if (options.missingIncludeState != INC_ERROR) {
		printDep(path);
	}
	return std::nullopt;
}

bool yywrap() {
	uint32_t ifDepth = lexer_GetIFDepth();

	if (ifDepth != 0) {
		fatal(
		    "Ended block with %" PRIu32 " unterminated IF construct%s",
		    ifDepth,
		    ifDepth == 1 ? "" : "s"
		);
	}

	if (Context &context = contextStack.top(); context.fileInfo->type == NODE_REPT) {
		// The context is a REPT or FOR block, which may loop

		// If the node is referenced outside this context, we can't edit it, so duplicate it
		if (context.fileInfo.use_count() > 1) {
			context.fileInfo = std::make_shared<FileStackNode>(*context.fileInfo);
			context.fileInfo->ID = UINT32_MAX; // The copy is not yet registered
		}

		std::vector<uint32_t> &fileInfoIters = context.fileInfo->iters();

		// If this is a FOR, update the symbol value
		if (context.isForLoop && fileInfoIters.front() <= context.nbReptIters) {
			// Avoid arithmetic overflow runtime error
			uint32_t forValue =
			    static_cast<uint32_t>(context.forValue) + static_cast<uint32_t>(context.forStep);
			context.forValue =
			    forValue <= INT32_MAX ? forValue : -static_cast<int32_t>(~forValue) - 1;
			Symbol *sym = sym_AddVar(context.forName, context.forValue);

			// This error message will refer to the current iteration
			if (sym->type != SYM_VAR) {
				fatal("Failed to update FOR symbol value");
			}
		}
		// Advance to the next iteration
		++fileInfoIters.front();
		// If this wasn't the last iteration, wrap instead of popping
		if (fileInfoIters.front() <= context.nbReptIters) {
			lexer_RestartRept(context.fileInfo->lineNo);
			context.uniqueIDStr->clear(); // Invalidate the current unique ID (if any).
			return false;
		}
	} else if (contextStack.size() == 1) {
		return true;
	}

	contextStack.pop();
	contextStack.top().lexerState.setAsCurrentState();

	return false;
}

static void checkRecursionDepth() {
	if (contextStack.size() > options.maxRecursionDepth) {
		fatal("Recursion limit (%zu) exceeded", options.maxRecursionDepth);
	}
}

static void newFileContext(std::string const &filePath, bool updateStateNow) {
	checkRecursionDepth();

	std::shared_ptr<std::string> uniqueIDStr = nullptr;
	std::shared_ptr<MacroArgs> macroArgs = nullptr;

	auto fileInfo =
	    std::make_shared<FileStackNode>(NODE_FILE, filePath == "-" ? "<stdin>" : filePath);
	if (!contextStack.empty()) {
		Context &oldContext = contextStack.top();
		fileInfo->parent = oldContext.fileInfo;
		fileInfo->lineNo = lexer_GetLineNo(); // Called before setting the lexer state
		uniqueIDStr = oldContext.uniqueIDStr; // Make a copy of the ID
		macroArgs = oldContext.macroArgs;
	}

	Context &context = contextStack.emplace(Context{
	    .fileInfo = fileInfo,
	    .uniqueIDStr = uniqueIDStr,
	    .macroArgs = macroArgs,
	});

	context.lexerState.setFileAsNextState(filePath, updateStateNow);
}

static void newMacroContext(Symbol const &macro, std::shared_ptr<MacroArgs> macroArgs) {
	checkRecursionDepth();

	Context &oldContext = contextStack.top();

	std::string fileInfoName;
	for (FileStackNode const *node = macro.src.get(); node; node = node->parent.get()) {
		if (node->type != NODE_REPT) {
			fileInfoName.append(node->name());
			break;
		}
	}
	if (macro.src->type == NODE_REPT) {
		fileInfoName.append(macro.src->reptChain());
	}
	fileInfoName.append("::");
	fileInfoName.append(macro.name);

	auto fileInfo = std::make_shared<FileStackNode>(NODE_MACRO, fileInfoName);
	assume(!contextStack.empty()); // The top level context cannot be a MACRO
	fileInfo->parent = oldContext.fileInfo;
	fileInfo->lineNo = lexer_GetLineNo();

	Context &context = contextStack.emplace(Context{
	    .fileInfo = fileInfo,
	    .uniqueIDStr = std::make_shared<std::string>(), // Create a new, not-yet-generated ID
	    .macroArgs = macroArgs,
	});

	context.lexerState.setViewAsNextState("MACRO", macro.getMacro(), macro.fileLine);
}

static Context &newReptContext(int32_t reptLineNo, ContentSpan const &span, uint32_t count) {
	checkRecursionDepth();

	Context &oldContext = contextStack.top();

	std::vector<uint32_t> fileInfoIters{1};
	if (oldContext.fileInfo->type == NODE_REPT && !oldContext.fileInfo->iters().empty()) {
		// Append all parent iter counts
		fileInfoIters.insert(fileInfoIters.end(), RANGE(oldContext.fileInfo->iters()));
	}

	auto fileInfo = std::make_shared<FileStackNode>(NODE_REPT, fileInfoIters);
	assume(!contextStack.empty()); // The top level context cannot be a REPT
	fileInfo->parent = oldContext.fileInfo;
	fileInfo->lineNo = reptLineNo;

	Context &context = contextStack.emplace(Context{
	    .fileInfo = fileInfo,
	    .uniqueIDStr = std::make_shared<std::string>(), // Create a new, not-yet-generated ID
	    .macroArgs = oldContext.macroArgs,
	});

	context.lexerState.setViewAsNextState("REPT", span, reptLineNo);

	context.nbReptIters = count;

	return context;
}

bool fstk_FileError(std::string const &path, char const *functionName) {
	if (options.missingIncludeState == INC_ERROR) {
		error("Error opening %s file '%s': %s", functionName, path.c_str(), strerror(errno));
	} else {
		failedOnMissingInclude = true;
		// LCOV_EXCL_START
		if (options.missingIncludeState == GEN_EXIT) {
			verbosePrint(
			    "Aborting (-MG) on %s file '%s' (%s)\n", functionName, path.c_str(), strerror(errno)
			);
			return true;
		}
		assume(options.missingIncludeState == GEN_CONTINUE);
		// LCOV_EXCL_STOP
	}
	return false;
}

bool fstk_FailedOnMissingInclude() {
	return failedOnMissingInclude;
}

bool fstk_RunInclude(std::string const &path) {
	if (std::optional<std::string> fullPath = fstk_FindFile(path); fullPath) {
		newFileContext(*fullPath, false);
		return false;
	}
	return fstk_FileError(path, "INCLUDE");
}

void fstk_RunMacro(std::string const &macroName, std::shared_ptr<MacroArgs> macroArgs) {
	Symbol *macro = sym_FindExactSymbol(macroName);

	if (!macro) {
		if (sym_IsPurgedExact(macroName)) {
			error("Macro \"%s\" not defined; it was purged", macroName.c_str());
		} else {
			error("Macro \"%s\" not defined", macroName.c_str());
		}
		return;
	}
	if (macro->type != SYM_MACRO) {
		error("\"%s\" is not a macro", macroName.c_str());
		return;
	}

	newMacroContext(*macro, macroArgs);
}

void fstk_RunRept(uint32_t count, int32_t reptLineNo, ContentSpan const &span) {
	if (count == 0) {
		return;
	}

	newReptContext(reptLineNo, span, count);
}

void fstk_RunFor(
    std::string const &symName,
    int32_t start,
    int32_t stop,
    int32_t step,
    int32_t reptLineNo,
    ContentSpan const &span
) {
	if (Symbol *sym = sym_AddVar(symName, start); sym->type != SYM_VAR) {
		return;
	}

	uint32_t count = 0;
	if (step > 0 && start < stop) {
		count = (static_cast<int64_t>(stop) - start - 1) / step + 1;
	} else if (step < 0 && stop < start) {
		count = (static_cast<int64_t>(start) - stop - 1) / -static_cast<int64_t>(step) + 1;
	} else if (step == 0) {
		error("FOR cannot have a step value of 0");
	}

	if ((step > 0 && start > stop) || (step < 0 && start < stop)) {
		warning(WARNING_BACKWARDS_FOR, "FOR goes backwards from %d to %d by %d", start, stop, step);
	}

	if (count == 0) {
		return;
	}

	Context &context = newReptContext(reptLineNo, span, count);
	context.isForLoop = true;
	context.forValue = start;
	context.forStep = step;
	context.forName = symName;
}

bool fstk_Break() {
	if (contextStack.top().fileInfo->type != NODE_REPT) {
		error("BREAK can only be used inside a REPT/FOR block");
		return false;
	}

	contextStack.top().nbReptIters = 0; // Prevent more iterations
	return true;
}

void fstk_NewRecursionDepth(size_t newDepth) {
	if (contextStack.size() > newDepth + 1) {
		fatal("Recursion limit (%zu) exceeded", newDepth);
	}
	options.maxRecursionDepth = newDepth;
}

void fstk_Init(std::string const &mainPath) {
	newFileContext(mainPath, true);

	for (std::string const &name : preIncludeNames) {
		if (std::optional<std::string> fullPath = fstk_FindFile(name); fullPath) {
			newFileContext(*fullPath, false);
		} else {
			error("Error reading pre-included file '%s': %s", name.c_str(), strerror(errno));
		}
	}
}
