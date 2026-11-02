// console_output — printing one JSON document to the server console in safely split chunks.
//
// s2sdk's PrintToServer is `ConMsg("%s\n", msg.c_str())` (s2sdk src/export/console.cpp:12-13), and
// tier0's ConMsg formats into a fixed 2048-byte buffer: everything past 2047 bytes is dropped and
// the call still appends its newline. A server with 18+ players produces a longer mm_getinfo
// document, so printing it in one call truncated the JSON mid-token and the web poller's json parse
// failed with EOF (confirmed live on 27035).
//
// The fix is to print the document in pieces that are small enough for one ConMsg call, cut ONLY
// between JSON tokens - immediately after `,` `:` `{` `[` `}` or `]` while outside a string. Each
// PrintToServer then appends a newline at a token boundary, which is legal JSON whitespace, so a
// client that concatenates the lines still parses exactly one object. A document that has a single
// token longer than the limit cannot be cut safely at all; it is replaced by one short error object
// rather than printed as a fragment.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <s2sdk/s2sdk.hpp>

namespace playersinfo {

// The most bytes one PrintToServer chunk may carry. ConMsg truncates at 2047 bytes including the
// newline it appends; 1800 leaves a wide margin and keeps the RCON lines well below the cap.
inline constexpr std::size_t kMaxConsoleChunkBytes = 1800;

// Printed instead of a document that cannot be split safely (one JSON token - a single string or
// number - longer than kMaxConsoleChunkBytes). It is a complete object, so the web poller still
// parses one answer and can log the problem.
inline constexpr std::string_view kDocumentTooLongJson = "{\"error\":\"document too long\"}";

// Characters after which a newline is legal JSON whitespace (a value or a member may start there,
// or a value has just ended) and which can never be part of a string token or an escape sequence.
[[nodiscard]] inline bool IsJsonSplitBoundary(char c) {
	return c == ',' || c == ':' || c == '{' || c == '[' || c == '}' || c == ']';
}

// Cuts `json` into byte-exact pieces of at most `maxBytes` bytes each, splitting only after a
// boundary character that is outside a string and not escaped. The pieces concatenate back to the
// whole document, so printing them one per line keeps the document intact.
//
// Returns false and clears `chunks` when the remaining document contains no boundary within
// `maxBytes` bytes (an indivisible token): the caller must not print a fragment then, it answers
// with kDocumentTooLongJson instead. A document that already fits stays one piece.
[[nodiscard]] inline bool SplitJsonForConsole(std::string_view json, std::size_t maxBytes,
											  std::vector<std::string_view>& chunks) {
	chunks.clear();
	if (maxBytes == 0) {
		return false;
	}
	if (json.size() <= maxBytes) {
		chunks.push_back(json);
		return true;
	}

	std::size_t start = 0;
	while (start < json.size()) {
		if (json.size() - start <= maxBytes) {
			chunks.push_back(json.substr(start));
			return true;
		}
		const std::size_t limit = start + maxBytes;
		// A chunk always starts at a token boundary, so the scan enters outside a string; the string
		// and escape state is tracked only to recognise the boundaries of this slice.
		bool inString = false;
		bool escaped = false;
		std::size_t cut = 0;
		for (std::size_t i = start; i < limit; ++i) {
			const char c = json[i];
			if (inString) {
				if (escaped) {
					escaped = false;
				} else if (c == '\\') {
					escaped = true;
				} else if (c == '"') {
					inString = false;
				}
				continue;
			}
			if (c == '"') {
				inString = true;
				continue;
			}
			if (IsJsonSplitBoundary(c)) {
				cut = i + 1; // the newline goes *after* the boundary character
			}
		}
		if (cut == 0) {
			chunks.clear();
			return false; // one token does not fit: never print a broken fragment
		}
		chunks.push_back(json.substr(start, cut - start));
		start = cut;
	}
	return true;
}

// Hands `json` to `print`, one call per chunk (see SplitJsonForConsole). Returns true when the
// document was printed; false when it had to be replaced by the error object, which was printed
// instead. The pieces are computed before anything is printed, so a document that cannot be split
// never reaches the console partially.
template <typename Print>
bool PrintJsonChunks(std::string_view json, std::size_t maxBytes, Print&& print) {
	std::vector<std::string_view> chunks;
	if (!SplitJsonForConsole(json, maxBytes, chunks)) {
		print(kDocumentTooLongJson);
		return false;
	}
	for (const std::string_view chunk : chunks) {
		print(chunk);
	}
	return true;
}

enum class ConsolePrintResult : std::uint8_t {
	NoBinding,           // s2sdk::PrintToServer is not available: nothing was printed at all
	PrintedDocument,     // the document went out in one or more chunks
	PrintedTooLongError, // the document was replaced by kDocumentTooLongJson
};

// The plugin's one way to put a JSON document on the server console. A missing s2sdk binding is a
// no-op (the server keeps running, the poll simply gets no answer), never a crash and never a
// partial document.
inline ConsolePrintResult PrintDocumentToServer(std::string_view json) {
	if (__s2sdk_PrintToServer == nullptr) {
		return ConsolePrintResult::NoBinding;
	}
	try {
		const bool printed = PrintJsonChunks(json, kMaxConsoleChunkBytes,
											 [](std::string_view chunk) {
												 s2sdk::PrintToServer(plg::string(chunk));
											 });
		return printed ? ConsolePrintResult::PrintedDocument : ConsolePrintResult::PrintedTooLongError;
	} catch (...) {
		// PrintToServer is a C export; a throwing plg::string allocation must not escape a command.
		return ConsolePrintResult::NoBinding;
	}
}

} // namespace playersinfo
