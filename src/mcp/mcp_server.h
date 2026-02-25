// Copyright (c) 2025, Aegisub Contributors
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace agi { struct Context; }

namespace mcp {

using json = nlohmann::json;

/// A single MCP tool definition
struct ToolDef {
	std::string name;
	std::string description;
	json input_schema; // JSON Schema for parameters
	std::function<json(const json& args, agi::Context* ctx)> handler;
	bool run_on_main_thread = true; // If false, handler runs on HTTP thread (for long HTTP calls)
};

/// MCP Server that runs an HTTP endpoint for AI agents
class McpServer {
	class Impl;
	std::unique_ptr<Impl> impl;

public:
	McpServer(agi::Context* context, int port = 6274);
	~McpServer();

	/// Start the HTTP server on a background thread
	void Start();

	/// Stop the HTTP server
	void Stop();

	/// Whether the server is currently running
	bool IsRunning() const;

	/// Get the port the server is listening on
	int GetPort() const;
};

/// Register all built-in MCP tools
std::vector<ToolDef> RegisterAllTools();

} // namespace mcp
