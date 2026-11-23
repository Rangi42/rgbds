// SPDX-License-Identifier: MIT

#ifndef RGBDS_BACKTRACE_HPP
#define RGBDS_BACKTRACE_HPP

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "helpers.hpp" // InvocableR
#include "itertools.hpp" // reversed
#include "linkdefs.hpp" // FileStackNodeType, NODE_REPT
#include "style.hpp"

#define TRACE_SEPARATOR  "<-"
#define NODE_SEPARATOR   "::"
#define REPT_NODE_PREFIX "REPT~"

struct Tracing {
	uint64_t depth = 0;
	bool collapse = false;
	bool loud = false;
};

extern Tracing tracing;

bool trace_ParseTraceDepth(char const *arg);

template<typename NodeT>
void trace_PrintBacktrace(
    std::vector<NodeT> const &stack,
    InvocableR<char const *, NodeT const &> auto getName,
    InvocableR<uint32_t, NodeT const &> auto getLineNo
) {
	size_t n = stack.size();
	if (n == 0) {
		return; // LCOV_EXCL_LINE
	}

	auto printLocation = [&](size_t i) {
		NodeT const &item = stack[n - i - 1];
		style_Reset(stderr);
		if (!tracing.collapse) {
			fputs("   ", stderr); // Just three spaces; the fourth will be printed next
		}
		fprintf(stderr, " %s ", i == 0 ? "at" : TRACE_SEPARATOR);
		style_Set(stderr, STYLE_CYAN, true);
		fputs(getName(item), stderr);
		style_Set(stderr, STYLE_CYAN, false);
		fprintf(stderr, "(%" PRIu32 ")", getLineNo(item));
		if (!tracing.collapse) {
			putc('\n', stderr);
		}
	};

	if (tracing.collapse) {
		fputs("   ", stderr); // Just three spaces; the fourth will be handled by the loop
	}

	if (tracing.depth == 0 || static_cast<size_t>(tracing.depth) >= n) {
		for (size_t i = 0; i < n; ++i) {
			printLocation(i);
		}
	} else {
		size_t last = tracing.depth / 2;
		size_t first = tracing.depth - last;
		size_t skipped = n - tracing.depth;
		for (size_t i = 0; i < first; ++i) {
			printLocation(i);
		}
		style_Reset(stderr);

		if (tracing.collapse) {
			fputs(" " TRACE_SEPARATOR, stderr);
		} else {
			fputs("   ", stderr); // Just three spaces; the fourth will be printed next
		}
		fprintf(stderr, " ...%zu more%s", skipped, last ? "..." : "");
		if (!tracing.collapse) {
			putc('\n', stderr);
		}

		for (size_t i = n - last; i < n; ++i) {
			printLocation(i);
		}
	}

	if (tracing.collapse) {
		putc('\n', stderr);
	}
	style_Reset(stderr);
}

// Walk a chain of file stack nodes and print the corresponding location backtrace.
// RGBASM and RGBLINK each have their own `FileStackNode` type (they differ in how they own their
// parents and in where they get their IDs from), but both expose the same information. Keeping the
// traversal here avoids two copies of the same fairly subtle logic (quiet nodes, REPT names, etc.).
template<typename NodeT>
void printFileStackBacktrace(NodeT const *node, uint32_t curLineNo) {
	using TraceItem = std::pair<NodeT const *, uint32_t>;
	std::vector<TraceItem> items;
	for (TraceItem item{node, curLineNo};;) {
		auto &[curNode, itemLineNo] = item;
		bool loud = !curNode->isQuiet || tracing.loud;
		if (loud) {
			items.emplace_back(curNode, itemLineNo);
		}
		if (!curNode->parentNode()) {
			assume(curNode->type != NODE_REPT && std::holds_alternative<std::string>(curNode->data));
			break;
		}
		if (loud || curNode->type != NODE_REPT) {
			// Quiet REPT nodes pass their interior line number up to their parent, which is more
			// precise than the parent's own line number (the line of the "REPT?" or "FOR?").
			itemLineNo = curNode->lineNo;
		}
		curNode = curNode->parentNode();
	}

	using TraceNode = std::pair<std::string, uint32_t>;
	std::vector<TraceNode> traceNodes;
	traceNodes.reserve(items.size());
	for (auto &[curNode, itemLineNo] : reversed(items)) {
		if (std::holds_alternative<std::vector<uint32_t>>(curNode->data)) {
			// REPT nodes use their first non-REPT parent's name (which may be a quiet node and
			// thus not in `traceNodes`), followed by their REPT parents' iteration counts,
			// followed by their own iteration count.
			std::string reptName;
			for (NodeT const *ancestor = curNode->parentNode();;
			     ancestor = ancestor->parentNode()) {
				// The top level context cannot be a REPT
				assume(ancestor != nullptr);
				if (ancestor->type != NODE_REPT) {
					reptName.append(ancestor->name());
					break;
				}
			}
			// Iteration counts are stored deepest-first, so print them outermost-first.
			for (uint32_t iter : reversed(curNode->iters())) {
				reptName.append(NODE_SEPARATOR REPT_NODE_PREFIX);
				reptName.append(std::to_string(iter));
			}
			traceNodes.emplace_back(std::move(reptName), itemLineNo);
		} else {
			traceNodes.emplace_back(curNode->name(), itemLineNo);
		}
	}

	trace_PrintBacktrace(
	    traceNodes,
	    [](TraceNode const &node) { return node.first.c_str(); },
	    [](TraceNode const &node) { return node.second; }
	);
}

#endif // RGBDS_BACKTRACE_HPP
