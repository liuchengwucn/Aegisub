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

#include <memory>
#include <string>

struct LLMRequest {
	std::string system_prompt;   // System instruction for the LLM
	std::string user_content;    // User text content (e.g. SRT subtitles + instructions)
	std::string audio_base64;    // Base64-encoded WAV audio data (optional, empty = no audio)
	std::string audio_mime_type; // MIME type, e.g. "audio/wav"
};

struct LLMResponse {
	std::string text;    // LLM response text
	bool success;        // Whether the call succeeded
	std::string error;   // Error message if !success
};

/// Abstract interface for multimodal LLM providers that support audio input
class LLMProvider {
public:
	virtual ~LLMProvider() = default;

	/// Send a request to the LLM and return the response. Thread-safe.
	virtual LLMResponse Call(const LLMRequest& request) = 0;

	/// Check if the provider has valid configuration (API key, etc.)
	virtual bool IsConfigured() const = 0;

	/// Get the provider name for display
	virtual std::string GetProviderName() const = 0;
};

/// Create an LLM provider based on current Aegisub configuration.
/// Reads "Automation/Audio LLM/Provider" to determine which implementation to use.
std::unique_ptr<LLMProvider> CreateLLMProvider();
