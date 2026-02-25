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

#include "llm_provider.h"
#include "options.h"

#include <libaegisub/log.h>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

using njson = nlohmann::json;

namespace {

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, std::string *s) {
	s->append(ptr, size * nmemb);
	return size * nmemb;
}

/// Helper to perform an HTTP POST with JSON body and return the response
static std::string HttpPostJson(const std::string& url,
                                const std::string& body,
                                const std::string& auth_header,
                                const std::string& proxy = "") {
	CURL *curl = curl_easy_init();
	if (!curl) return "";

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!auth_header.empty())
		headers = curl_slist_append(headers, auth_header.c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 minute timeout for large audio

	if (!proxy.empty())
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

	LOG_D("llm") << "POST " << url.substr(0, 80) << "... body_size=" << body.size();

	std::string response;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	CURLcode res = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		LOG_E("llm") << "CURL error (" << res << "): " << curl_easy_strerror(res) << " url=" << url.substr(0, 80);
		return "";
	}
	LOG_D("llm") << "Response size=" << response.size();
	return response;
}

// ============================================================
// Gemini LLM Provider
// ============================================================

class GeminiLLMProvider final : public LLMProvider {
public:
	LLMResponse Call(const LLMRequest& request) override {
		std::string base_url = OPT_GET("Automation/Audio LLM/Base URL")->GetString();
		std::string api_key = OPT_GET("Automation/Audio LLM/API Key")->GetString();
		std::string model = OPT_GET("Automation/Audio LLM/Model")->GetString();

		if (api_key.empty() || base_url.empty())
			return {"", false, "Gemini API key or base URL not configured"};

		// Build URL: {base_url}/models/{model}:generateContent?key={api_key}
		std::string url = base_url + "/models/" + model + ":generateContent?key=" + api_key;

		// Build request JSON
		njson parts = njson::array();

		// Add audio part if present
		if (!request.audio_base64.empty()) {
			parts.push_back({
				{"inlineData", {
					{"mimeType", request.audio_mime_type.empty() ? "audio/wav" : request.audio_mime_type},
					{"data", request.audio_base64}
				}}
			});
		}

		// Add text part
		std::string text = request.user_content;
		parts.push_back({{"text", text}});

		njson body = {
			{"contents", njson::array({
				{{"parts", parts}}
			})}
		};

		// Add system instruction if provided
		if (!request.system_prompt.empty()) {
			body["systemInstruction"] = {
				{"parts", njson::array({
					{{"text", request.system_prompt}}
				})}
			};
		}

		std::string proxy = OPT_GET("Automation/Audio LLM/HTTP Proxy")->GetString();
		std::string response_str = HttpPostJson(url, body.dump(), "", proxy);
		if (response_str.empty())
			return {"", false, "Empty response from Gemini API"};

		// Parse response
		try {
			njson resp = njson::parse(response_str);

			// Check for error
			if (resp.contains("error")) {
				std::string msg = resp["error"].value("message", "Unknown Gemini API error");
				return {"", false, msg};
			}

			// Extract text from candidates[0].content.parts[].text
			if (resp.contains("candidates") && !resp["candidates"].empty()) {
				auto& parts = resp["candidates"][0]["content"]["parts"];
				std::string result;
				for (auto& part : parts) {
					if (part.contains("text"))
						result += part["text"].get<std::string>();
				}
				return {result, true, ""};
			}

			return {"", false, "No candidates in Gemini response"};
		} catch (const std::exception& e) {
			LOG_E("llm") << "Failed to parse Gemini response: " << e.what();
			return {"", false, std::string("Failed to parse response: ") + e.what()};
		}
	}

	bool IsConfigured() const override {
		std::string api_key = OPT_GET("Automation/Audio LLM/API Key")->GetString();
		std::string base_url = OPT_GET("Automation/Audio LLM/Base URL")->GetString();
		return !api_key.empty() && !base_url.empty();
	}

	std::string GetProviderName() const override { return "gemini"; }
};

// ============================================================
// OpenAI-compatible LLM Provider
// ============================================================

class OpenAILLMProvider final : public LLMProvider {
public:
	LLMResponse Call(const LLMRequest& request) override {
		std::string base_url = OPT_GET("Automation/Audio LLM/Base URL")->GetString();
		std::string api_key = OPT_GET("Automation/Audio LLM/API Key")->GetString();
		std::string model = OPT_GET("Automation/Audio LLM/Model")->GetString();

		if (api_key.empty() || base_url.empty())
			return {"", false, "OpenAI API key or base URL not configured"};

		std::string url = base_url + "/chat/completions";

		// Build messages
		njson messages = njson::array();

		// System message
		if (!request.system_prompt.empty()) {
			messages.push_back({
				{"role", "system"},
				{"content", request.system_prompt}
			});
		}

		// User message with optional audio
		njson user_content = njson::array();

		if (!request.audio_base64.empty()) {
			user_content.push_back({
				{"type", "input_audio"},
				{"input_audio", {
					{"data", request.audio_base64},
					{"format", "wav"}
				}}
			});
		}

		user_content.push_back({
			{"type", "text"},
			{"text", request.user_content}
		});

		messages.push_back({
			{"role", "user"},
			{"content", user_content}
		});

		njson body = {
			{"model", model},
			{"messages", messages}
		};

		std::string auth = "Authorization: Bearer " + api_key;
		std::string proxy = OPT_GET("Automation/Audio LLM/HTTP Proxy")->GetString();
		std::string response_str = HttpPostJson(url, body.dump(), auth, proxy);
		if (response_str.empty())
			return {"", false, "Empty response from OpenAI API"};

		// Parse response
		try {
			njson resp = njson::parse(response_str);

			if (resp.contains("error")) {
				std::string msg = resp["error"].value("message", "Unknown OpenAI API error");
				return {"", false, msg};
			}

			if (resp.contains("choices") && !resp["choices"].empty()) {
				return {resp["choices"][0]["message"]["content"].get<std::string>(), true, ""};
			}

			return {"", false, "No choices in OpenAI response"};
		} catch (const std::exception& e) {
			LOG_E("llm") << "Failed to parse OpenAI response: " << e.what();
			return {"", false, std::string("Failed to parse response: ") + e.what()};
		}
	}

	bool IsConfigured() const override {
		std::string api_key = OPT_GET("Automation/Audio LLM/API Key")->GetString();
		std::string base_url = OPT_GET("Automation/Audio LLM/Base URL")->GetString();
		return !api_key.empty() && !base_url.empty();
	}

	std::string GetProviderName() const override { return "openai"; }
};

} // anonymous namespace

std::unique_ptr<LLMProvider> CreateLLMProvider() {
	std::string provider = OPT_GET("Automation/Audio LLM/Provider")->GetString();
	if (provider == "openai")
		return std::make_unique<OpenAILLMProvider>();
	// Default to Gemini
	return std::make_unique<GeminiLLMProvider>();
}
