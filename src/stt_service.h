// Copyright (c) 2025, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace agi { struct Context; }
class AssDialogue;
class STTProvider;

/// Manages speech-to-text transcription for subtitle lines
class STTService {
	agi::Context *context;

	/// The current STT provider implementation
	std::unique_ptr<STTProvider> provider;

	/// In-memory cache: dialogue line Id -> transcribed text
	std::map<int, std::string> cache;

	/// Set of line Ids currently being transcribed
	std::set<int> in_flight;

	/// Mutex protecting cache and in_flight
	mutable std::mutex mutex;

	static constexpr const char *EXTRADATA_KEY = "stt";
	static constexpr int MAX_DURATION_MS = 60000;

	/// Atomic counter for generating unique temp file names
	static std::atomic<int> temp_file_counter;

	void RecreateProvider();
	void StoreInExtradata(AssDialogue *line, std::string const& text);

public:
	STTService(agi::Context *context);
	~STTService();

	std::string GetCachedText(AssDialogue const *line) const;
	bool HasText(AssDialogue const *line) const;
	void TranscribeAsync(AssDialogue *line, std::function<void(std::string const&)> on_complete);
	void TranscribeAsync(AssDialogue *line, int start_ms, int end_ms, std::function<void(std::string const&)> on_complete);
	void TranscribeWithLookahead(AssDialogue *line, std::function<void(std::string const&)> on_active_complete);
	void InvalidateCache(AssDialogue *line);
	void LoadFromExtradata();
	void Clear();
};
