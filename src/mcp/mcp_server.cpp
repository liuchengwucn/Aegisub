// Copyright (c) 2025, Aegisub Contributors
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// Aegisub Project http://www.aegisub.org/

#include "mcp_server.h"

#include "include/aegisub/context.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/log.h>

#include <httplib.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

namespace mcp {

static const char* MCP_PROTOCOL_VERSION = "2024-11-05";

class McpServer::Impl {
public:
	agi::Context* context;
	int port;
	httplib::Server server;
	std::thread server_thread;
	std::atomic<bool> running{false};
	std::vector<ToolDef> tools;
	std::unordered_map<std::string, ToolDef*> tool_map;
	std::mutex session_mutex;
	bool initialized = false;

	Impl(agi::Context* ctx, int port) : context(ctx), port(port) {
		tools = RegisterAllTools();
		for (auto& tool : tools)
			tool_map[tool.name] = &tool;
		SetupRoutes();
	}

	void SetupRoutes() {
		// MCP HTTP Streamable transport endpoint
		server.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
			HandleMcpRequest(req, res);
		});

		// Health check
		server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
			res.set_content(R"({"status":"ok"})", "application/json");
		});
	}

	void HandleMcpRequest(const httplib::Request& req, httplib::Response& res) {
		json request;
		try {
			request = json::parse(req.body);
		} catch (const json::parse_error& e) {
			SendJsonRpcError(res, json(), -32700, "Parse error: " + std::string(e.what()));
			return;
		}

		// Handle single request
		if (request.is_object()) {
			json response = ProcessJsonRpc(request);
			// Notifications (no "id") get no response
			if (response.is_null()) {
				res.status = 202;
				return;
			}
			res.set_content(response.dump(), "application/json");
			return;
		}

		// Handle batch request
		if (request.is_array()) {
			json responses = json::array();
			for (auto& req_item : request) {
				json response = ProcessJsonRpc(req_item);
				if (!response.is_null())
					responses.push_back(std::move(response));
			}
			if (responses.empty()) {
				res.status = 202;
				return;
			}
			res.set_content(responses.dump(), "application/json");
			return;
		}

		SendJsonRpcError(res, json(), -32600, "Invalid Request");
	}

	json ProcessJsonRpc(const json& request) {
		// Validate JSON-RPC 2.0
		if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
			return MakeError(request.value("id", json()), -32600, "Invalid Request: missing jsonrpc 2.0");
		}

		if (!request.contains("method") || !request["method"].is_string()) {
			return MakeError(request.value("id", json()), -32600, "Invalid Request: missing method");
		}

		std::string method = request["method"];
		json id = request.value("id", json());
		json params = request.value("params", json::object());

		// Notification (no id) - process but don't respond
		bool is_notification = !request.contains("id");

		json result;
		try {
			if (method == "initialize") {
				result = HandleInitialize(params);
			} else if (method == "notifications/initialized") {
				// Client acknowledges initialization - no response needed
				return json();
			} else if (method == "ping") {
				result = json::object();
			} else if (method == "tools/list") {
				result = HandleToolsList(params);
			} else if (method == "tools/call") {
				result = HandleToolsCall(params);
			} else {
				if (is_notification) return json();
				return MakeError(id, -32601, "Method not found: " + method);
			}
		} catch (const std::exception& e) {
			if (is_notification) return json();
			return MakeError(id, -32603, std::string("Internal error: ") + e.what());
		}

		if (is_notification) return json();

		return {
			{"jsonrpc", "2.0"},
			{"id", id},
			{"result", result}
		};
	}

	json HandleInitialize(const json& /*params*/) {
		std::lock_guard<std::mutex> lock(session_mutex);
		initialized = true;

		return {
			{"protocolVersion", MCP_PROTOCOL_VERSION},
			{"capabilities", {
				{"tools", json::object()}
			}},
			{"serverInfo", {
				{"name", "aegisub"},
				{"version", "3.4.1"}
			}}
		};
	}

	json HandleToolsList(const json& /*params*/) {
		json tool_list = json::array();
		for (auto& tool : tools) {
			tool_list.push_back({
				{"name", tool.name},
				{"description", tool.description},
				{"inputSchema", tool.input_schema}
			});
		}
		return {{"tools", tool_list}};
	}

	json HandleToolsCall(const json& params) {
		if (!params.contains("name") || !params["name"].is_string()) {
			throw std::runtime_error("Missing tool name");
		}

		std::string name = params["name"];
		json arguments = params.value("arguments", json::object());

		auto it = tool_map.find(name);
		if (it == tool_map.end()) {
			return {
				{"content", json::array({
					{{"type", "text"}, {"text", "Unknown tool: " + name}}
				})},
				{"isError", true}
			};
		}

		try {
			json tool_result;
			std::exception_ptr eptr;

			if (it->second->run_on_main_thread) {
				// Most tools run on GUI thread for thread safety
				agi::dispatch::Main().Sync([&] {
					try {
						tool_result = it->second->handler(arguments, context);
					} catch (...) {
						eptr = std::current_exception();
					}
				});
			} else {
				// Long-running tools (e.g. HTTP API calls) run on HTTP thread;
				// they dispatch to GUI thread internally as needed
				try {
					tool_result = it->second->handler(arguments, context);
				} catch (...) {
					eptr = std::current_exception();
				}
			}

			if (eptr) std::rethrow_exception(eptr);

			// If the tool returned a raw result, wrap it in MCP content format
			if (!tool_result.contains("content")) {
				return {
					{"content", json::array({
						{{"type", "text"}, {"text", tool_result.dump()}}
					})}
				};
			}
			return tool_result;

		} catch (const std::exception& e) {
			return {
				{"content", json::array({
					{{"type", "text"}, {"text", std::string("Error: ") + e.what()}}
				})},
				{"isError", true}
			};
		}
	}

	static json MakeError(const json& id, int code, const std::string& message) {
		return {
			{"jsonrpc", "2.0"},
			{"id", id},
			{"error", {
				{"code", code},
				{"message", message}
			}}
		};
	}

	static void SendJsonRpcError(httplib::Response& res, const json& id, int code, const std::string& message) {
		res.set_content(MakeError(id, code, message).dump(), "application/json");
	}
};

McpServer::McpServer(agi::Context* context, int port)
	: impl(std::make_unique<Impl>(context, port)) {}

McpServer::~McpServer() {
	Stop();
}

void McpServer::Start() {
	if (impl->running) return;

	impl->running = true;
	impl->server_thread = std::thread([this] {
		LOG_I("mcp/server") << "MCP server starting on port " << impl->port;
		if (!impl->server.listen("127.0.0.1", impl->port)) {
			LOG_E("mcp/server") << "MCP server failed to start on port " << impl->port;
		}
		impl->running = false;
		LOG_I("mcp/server") << "MCP server stopped";
	});
}

void McpServer::Stop() {
	if (!impl->running) return;
	impl->server.stop();
	if (impl->server_thread.joinable())
		impl->server_thread.join();
}

bool McpServer::IsRunning() const {
	return impl->running;
}

int McpServer::GetPort() const {
	return impl->port;
}

} // namespace mcp
