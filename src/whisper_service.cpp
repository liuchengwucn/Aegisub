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

#include "whisper_service.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <filesystem>

#include <curl/curl.h>

#include <wx/intl.h>

WhisperService::WhisperService(agi::Context *context)
: context(context)
{
}

void WhisperService::LoadFromExtradata() {
	std::lock_guard<std::mutex> lock(mutex);
	cache.clear();

	for (auto& line : context->ass->Events) {
		auto ids = line.ExtradataIds.get();
		if (ids.empty()) continue;

		auto entries = context->ass->GetExtradata(ids);
		for (auto const& e : entries) {
			if (e.key == EXTRADATA_KEY && !e.value.empty()) {
				cache[line.Id] = e.value;
				break;
			}
		}
	}
}

std::string WhisperService::GetCachedText(AssDialogue const *line) const {
	std::lock_guard<std::mutex> lock(mutex);
	auto it = cache.find(line->Id);
	if (it != cache.end())
		return it->second;
	return "";
}

bool WhisperService::HasText(AssDialogue const *line) const {
	std::lock_guard<std::mutex> lock(mutex);
	return cache.count(line->Id) > 0;
}

void WhisperService::Clear() {
	std::lock_guard<std::mutex> lock(mutex);
	cache.clear();
	in_flight.clear();
}

void WhisperService::StoreInExtradata(AssDialogue *line, std::string const& text) {
	uint32_t id = context->ass->AddExtradata(EXTRADATA_KEY, text);
	auto ids = line->ExtradataIds.get();

	// Remove any existing whisper extradata
	auto existing = context->ass->GetExtradata(ids);
	ids.erase(std::remove_if(ids.begin(), ids.end(), [&](uint32_t eid) {
		for (auto& e : existing)
			if (e.id == eid && e.key == EXTRADATA_KEY) return true;
		return false;
	}), ids.end());

	ids.push_back(id);
	line->ExtradataIds = ids;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, std::string *s) {
	s->append(ptr, size * nmemb);
	return size * nmemb;
}

std::string WhisperService::CallWhisperAPI(std::string const& wav_path) {
	std::string base_url = OPT_GET("Automation/Whisper/Base URL")->GetString();
	std::string api_key = OPT_GET("Automation/Whisper/API Key")->GetString();
	std::string model = OPT_GET("Automation/Whisper/Model")->GetString();
	std::string language = OPT_GET("Automation/Whisper/Language")->GetString();

	if (api_key.empty() || base_url.empty()) return "";

	CURL *curl = curl_easy_init();
	if (!curl) return "";

	std::string url = base_url + "/audio/transcriptions";

	curl_mime *mime = curl_mime_init(curl);
	curl_mimepart *part;

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "file");
	curl_mime_filedata(part, wav_path.c_str());

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "model");
	curl_mime_data(part, model.c_str(), CURL_ZERO_TERMINATED);

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "response_format");
	curl_mime_data(part, "text", CURL_ZERO_TERMINATED);

	if (!language.empty() && language != "Auto") {
		part = curl_mime_addpart(mime);
		curl_mime_name(part, "language");
		curl_mime_data(part, language.c_str(), CURL_ZERO_TERMINATED);
	}

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	std::string response;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	CURLcode res = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_mime_free(mime);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		LOG_E("whisper") << "CURL error: " << curl_easy_strerror(res);
		return "";
	}

	// Try to extract "text" field if response is JSON
	// Handles both {"text":"..."} format and plain text format
	auto text_pos = response.find("\"text\"");
	if (text_pos != std::string::npos) {
		// Find the opening quote of the value
		auto val_start = response.find('"', text_pos + 6);
		if (val_start != std::string::npos) {
			val_start++; // skip the opening quote
			std::string result;
			for (size_t i = val_start; i < response.size(); i++) {
				if (response[i] == '\\' && i + 1 < response.size()) {
					char next = response[i + 1];
					if (next == '"') { result += '"'; i++; }
					else if (next == '\\') { result += '\\'; i++; }
					else if (next == 'n') { result += '\n'; i++; }
					else if (next == 't') { result += '\t'; i++; }
					else { result += next; i++; }
				} else if (response[i] == '"') {
					break;
				} else {
					result += response[i];
				}
			}
			return result;
		}
	}

	// Trim trailing whitespace/newlines
	while (!response.empty() && (response.back() == '\n' || response.back() == '\r' || response.back() == ' '))
		response.pop_back();

	return response;
}

void WhisperService::TranscribeAsync(AssDialogue *line, std::function<void(std::string const&)> on_complete) {
	if (!line) return;
	TranscribeAsync(line, line->Start, line->End, std::move(on_complete));
}

void WhisperService::TranscribeAsync(AssDialogue *line, int start_ms, int end_ms, std::function<void(std::string const&)> on_complete) {
	if (!line) return;

	std::string api_key = OPT_GET("Automation/Whisper/API Key")->GetString();
	if (api_key.empty()) return;

	auto provider = context->project->AudioProvider();
	if (!provider) return;

	int duration_ms = end_ms - start_ms;
	if (duration_ms <= 0 || duration_ms > MAX_DURATION_MS) return;

	int line_id = line->Id;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (cache.count(line_id) > 0) return;
		if (in_flight.count(line_id) > 0) return;
		in_flight.insert(line_id);
	}

	agi::dispatch::Background().Async([=, this]() {
		// Export audio clip to temp WAV
		auto temp_dir = std::filesystem::temp_directory_path();
		auto temp_path = temp_dir / ("aegisub_whisper_" + std::to_string(line_id) + ".wav");
		agi::fs::path wav_path(temp_path.string());

		try {
			agi::SaveAudioClip(*provider, wav_path, start_ms, end_ms);
		} catch (std::exception const& e) {
			LOG_E("whisper") << "Failed to export audio: " << e.what();
			std::lock_guard<std::mutex> lock(mutex);
			in_flight.erase(line_id);
			agi::dispatch::Main().Async([on_complete]() {
				on_complete("");
			});
			return;
		}

		std::string result = CallWhisperAPI(temp_path.string());

		// Clean up temp file
		try { std::filesystem::remove(temp_path); } catch (...) {}

		agi::dispatch::Main().Async([=, this]() {
			{
				std::lock_guard<std::mutex> lock(mutex);
				in_flight.erase(line_id);
				if (!result.empty())
					cache[line_id] = result;
			}

			// Store in extradata if we got a result
			if (!result.empty()) {
				// Find the dialogue line by Id
				for (auto& d : context->ass->Events) {
					if (d.Id == line_id) {
						StoreInExtradata(&d, result);
						context->ass->Commit(_("whisper transcription"),
							AssFile::COMMIT_EXTRADATA | AssFile::COMMIT_DIAG_META);
						break;
					}
				}
			}

			on_complete(result);
		});
	});
}

void WhisperService::TranscribeWithLookahead(AssDialogue *line, std::function<void(std::string const&)> on_active_complete) {
	if (!line) return;

	// Transcribe the active line with the UI callback
	TranscribeAsync(line, std::move(on_active_complete));

	// Lookahead: silently transcribe subsequent lines
	int lookahead = OPT_GET("Automation/Whisper/Lookahead Lines")->GetInt();
	if (lookahead <= 0) return;

	auto it = context->ass->iterator_to(*line);
	++it; // move past current line
	for (int i = 0; i < lookahead && it != context->ass->Events.end(); ++i, ++it) {
		AssDialogue *next = &*it;
		TranscribeAsync(next, [](std::string const&) {});
	}
}

void WhisperService::InvalidateCache(AssDialogue *line) {
	if (!line) return;
	std::lock_guard<std::mutex> lock(mutex);
	cache.erase(line->Id);
}
