// Copyright (c) 2025, Aegisub Contributors
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// Aegisub Project http://www.aegisub.org/

#include "mcp_server.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_karaoke.h"
#include "ass_style.h"
#include "async_video_provider.h"
#include "auto4_base.h"
#include "command/command.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "resolution_resampler.h"
#include "search_replace_engine.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "subtitle_format.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/ass/karaoke.h>
#include <libaegisub/ass/time.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/color.h>
#include <libaegisub/character_count.h>
#include <libaegisub/vfr.h>

#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/string.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

namespace mcp {

using json = nlohmann::json;

// ============================================================
// Helpers
// ============================================================

static json DialogueToJson(const AssDialogue& line, int index) {
	return {
		{"index", index},
		{"start_time", (int)line.Start},
		{"end_time", (int)line.End},
		{"style", line.Style.get()},
		{"actor", line.Actor.get()},
		{"text", line.Text.get()},
		{"text_stripped", line.GetStrippedText()},
		{"effect", line.Effect.get()},
		{"comment", line.Comment},
		{"layer", line.Layer},
		{"margin_l", line.Margin[0]},
		{"margin_r", line.Margin[1]},
		{"margin_t", line.Margin[2]}
	};
}

static json StyleToJson(const AssStyle& style) {
	return {
		{"name", style.name},
		{"fontname", style.font},
		{"fontsize", style.fontsize},
		{"color1", style.primary.GetAssStyleFormatted()},
		{"color2", style.secondary.GetAssStyleFormatted()},
		{"color3", style.outline.GetAssStyleFormatted()},
		{"color4", style.shadow.GetAssStyleFormatted()},
		{"bold", style.bold},
		{"italic", style.italic},
		{"underline", style.underline},
		{"strikeout", style.strikeout},
		{"scale_x", style.scalex},
		{"scale_y", style.scaley},
		{"spacing", style.spacing},
		{"angle", style.angle},
		{"borderstyle", style.borderstyle},
		{"outline", style.outline_w},
		{"shadow", style.shadow_w},
		{"alignment", style.alignment},
		{"margin_l", style.Margin[0]},
		{"margin_r", style.Margin[1]},
		{"margin_t", style.Margin[2]},
		{"encoding", style.encoding}
	};
}

static AssDialogue* GetLineByIndex(agi::Context* ctx, int index) {
	int i = 0;
	for (auto& line : ctx->ass->Events) {
		if (i == index) return &line;
		++i;
	}
	return nullptr;
}

static int CountLines(agi::Context* ctx) {
	int count = 0;
	for ([[maybe_unused]] auto& line : ctx->ass->Events)
		++count;
	return count;
}

static AssStyle* FindStyleByName(agi::Context* ctx, const std::string& name) {
	for (auto& style : ctx->ass->Styles)
		if (style.name == name) return &style;
	return nullptr;
}

static void ApplyStyleProps(AssStyle* style, const json& args) {
	if (args.contains("fontname")) style->font = args["fontname"].get<std::string>();
	if (args.contains("fontsize")) style->fontsize = args["fontsize"];
	if (args.contains("bold")) style->bold = args["bold"];
	if (args.contains("italic")) style->italic = args["italic"];
	if (args.contains("underline")) style->underline = args["underline"];
	if (args.contains("strikeout")) style->strikeout = args["strikeout"];
	if (args.contains("scale_x")) style->scalex = args["scale_x"];
	if (args.contains("scale_y")) style->scaley = args["scale_y"];
	if (args.contains("spacing")) style->spacing = args["spacing"];
	if (args.contains("angle")) style->angle = args["angle"];
	if (args.contains("borderstyle")) style->borderstyle = args["borderstyle"];
	if (args.contains("outline")) style->outline_w = args["outline"];
	if (args.contains("shadow")) style->shadow_w = args["shadow"];
	if (args.contains("alignment")) style->alignment = args["alignment"];
	if (args.contains("encoding")) style->encoding = args["encoding"];
	if (args.contains("margin_l")) style->Margin[0] = args["margin_l"];
	if (args.contains("margin_r")) style->Margin[1] = args["margin_r"];
	if (args.contains("margin_t")) style->Margin[2] = args["margin_t"];
	if (args.contains("color1")) style->primary = agi::Color(args["color1"].get<std::string>());
	if (args.contains("color3")) style->outline = agi::Color(args["color3"].get<std::string>());
	if (args.contains("color4")) style->shadow = agi::Color(args["color4"].get<std::string>());
}

static std::string Base64Encode(const void* data, size_t len) {
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	auto bytes = static_cast<const uint8_t*>(data);
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = (uint32_t)bytes[i] << 16;
		if (i + 1 < len) n |= (uint32_t)bytes[i + 1] << 8;
		if (i + 2 < len) n |= (uint32_t)bytes[i + 2];
		out.push_back(table[(n >> 18) & 0x3F]);
		out.push_back(table[(n >> 12) & 0x3F]);
		out.push_back(i + 1 < len ? table[(n >> 6) & 0x3F] : '=');
		out.push_back(i + 2 < len ? table[n & 0x3F] : '=');
	}
	return out;
}

// ============================================================
// Tool 1: project — Project & metadata operations
// ============================================================

static ToolDef MakeProjectTool() {
	return {
		"project",
		"Project & metadata operations.\n"
		"Actions:\n"
		"- get_info: Get project info (version, line/style count, video/audio status, resolution, script_info)\n"
		"- set_script_info: Set a script info key/value (e.g. Title, PlayResX)\n"
		"- load_media: Load video and/or audio files\n"
		"- resample_resolution: Resample subtitle positions/sizes to a new resolution",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get_info", "set_script_info", "load_media", "resample_resolution"}},
						{"description", "Operation to perform"}}},
			{"key", {{"type", "string"}, {"description", "Script info key (for set_script_info)"}}},
			{"value", {{"type", "string"}, {"description", "Script info value (for set_script_info)"}}},
			{"video_path", {{"type", "string"}, {"description", "Video file path (for load_media)"}}},
			{"audio_path", {{"type", "string"}, {"description", "Audio file path (for load_media)"}}},
			{"source_x", {{"type", "integer"}, {"description", "Original X resolution (for resample_resolution)"}}},
			{"source_y", {{"type", "integer"}, {"description", "Original Y resolution (for resample_resolution)"}}},
			{"dest_x", {{"type", "integer"}, {"description", "New X resolution (for resample_resolution)"}}},
			{"dest_y", {{"type", "integer"}, {"description", "New Y resolution (for resample_resolution)"}}},
			{"ar_mode", {{"type", "string"}, {"enum", {"stretch", "add_border", "remove_border"}}, {"description", "Aspect ratio mode (for resample_resolution)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];

			if (action == "get_info") {
				json result = {
					{"version", "3.4.1"},
					{"has_subtitles", true},
					{"line_count", CountLines(ctx)},
					{"style_count", (int)std::distance(ctx->ass->Styles.begin(), ctx->ass->Styles.end())}
				};
				auto* vp = ctx->project->VideoProvider();
				result["has_video"] = vp != nullptr;
				if (vp) {
					result["video_file"] = ctx->project->VideoName().string();
					result["video_width"] = vp->GetWidth();
					result["video_height"] = vp->GetHeight();
					result["video_frame_count"] = vp->GetFrameCount();
					result["video_fps"] = vp->GetFPS().FPS();
				}
				result["has_audio"] = ctx->project->AudioProvider() != nullptr;
				if (ctx->project->AudioProvider())
					result["audio_file"] = ctx->project->AudioName().string();
				result["subtitle_file"] = ctx->subsController->Filename().string();
				int w = 0, h = 0;
				ctx->ass->GetResolution(w, h);
				result["resolution_x"] = w;
				result["resolution_y"] = h;
				json info = json::object();
				for (auto& entry : ctx->ass->Info)
					info[entry.Key()] = entry.Value();
				result["script_info"] = info;
				return result;
			}
			else if (action == "set_script_info") {
				std::string key = args.at("key");
				std::string value = args.at("value");
				ctx->ass->SetScriptInfo(key, value);
				ctx->ass->Commit("MCP: set script info", AssFile::COMMIT_SCRIPTINFO);
				return {{"key", key}, {"value", value}, {"set", true}};
			}
			else if (action == "load_media") {
				json result;
				if (args.contains("video_path")) {
					std::string vpath = args["video_path"];
					ctx->project->LoadVideo(agi::fs::path(vpath));
					result["video_loaded"] = vpath;
				}
				if (args.contains("audio_path")) {
					std::string apath = args["audio_path"];
					ctx->project->LoadAudio(agi::fs::path(apath));
					result["audio_loaded"] = apath;
				}
				if (result.empty())
					throw std::runtime_error("No video_path or audio_path provided");
				return result;
			}
			else if (action == "resample_resolution") {
				ResampleSettings settings;
				settings.source_x = args.at("source_x");
				settings.source_y = args.at("source_y");
				settings.dest_x = args.at("dest_x");
				settings.dest_y = args.at("dest_y");
				settings.margin[0] = settings.margin[1] = settings.margin[2] = settings.margin[3] = 0;
				std::string mode = args.value("ar_mode", std::string("stretch"));
				if (mode == "add_border") settings.ar_mode = ResampleARMode::AddBorder;
				else if (mode == "remove_border") settings.ar_mode = ResampleARMode::RemoveBorder;
				else settings.ar_mode = ResampleARMode::Stretch;
				settings.source_matrix = YCbCrMatrix::tv_709;
				settings.dest_matrix = YCbCrMatrix::tv_709;
				ResampleResolution(ctx->ass.get(), settings);
				ctx->ass->Commit("MCP: resample resolution",
					AssFile::COMMIT_SCRIPTINFO | AssFile::COMMIT_DIAG_META | AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_STYLES);
				return {{"resampled", true}, {"dest_x", settings.dest_x}, {"dest_y", settings.dest_y}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 2: styles — Style management
// ============================================================

static ToolDef MakeStylesTool() {
	return {
		"styles",
		"Subtitle style management.\n"
		"Actions:\n"
		"- list: Get all style definitions\n"
		"- create: Create a new style (name required, other props optional)\n"
		"- update: Update an existing style by name (partial update)",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"list", "create", "update"}},
						{"description", "Operation to perform"}}},
			{"name", {{"type", "string"}, {"description", "Style name (for create/update)"}}},
			{"fontname", {{"type", "string"}}}, {"fontsize", {{"type", "number"}}},
			{"color1", {{"type", "string"}, {"description", "Primary color (ASS format)"}}},
			{"color3", {{"type", "string"}, {"description", "Outline color"}}},
			{"color4", {{"type", "string"}, {"description", "Shadow color"}}},
			{"bold", {{"type", "boolean"}}}, {"italic", {{"type", "boolean"}}},
			{"underline", {{"type", "boolean"}}}, {"strikeout", {{"type", "boolean"}}},
			{"scale_x", {{"type", "number"}}}, {"scale_y", {{"type", "number"}}},
			{"spacing", {{"type", "number"}}}, {"angle", {{"type", "number"}}},
			{"borderstyle", {{"type", "integer"}}},
			{"outline", {{"type", "number"}}}, {"shadow", {{"type", "number"}}},
			{"alignment", {{"type", "integer"}}},
			{"margin_l", {{"type", "integer"}}}, {"margin_r", {{"type", "integer"}}}, {"margin_t", {{"type", "integer"}}},
			{"encoding", {{"type", "integer"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];

			if (action == "list") {
				json styles = json::array();
				for (auto& style : ctx->ass->Styles)
					styles.push_back(StyleToJson(style));
				return {{"styles", styles}};
			}
			else if (action == "create") {
				std::string name = args.at("name");
				if (FindStyleByName(ctx, name))
					throw std::runtime_error("Style already exists: " + name);
				auto* style = new AssStyle;
				style->name = name;
				ApplyStyleProps(style, args);
				style->UpdateData();
				ctx->ass->Styles.push_back(*style);
				ctx->ass->Commit("MCP: create style", AssFile::COMMIT_STYLES);
				return {{"name", name}, {"created", true}};
			}
			else if (action == "update") {
				std::string name = args.at("name");
				auto* style = FindStyleByName(ctx, name);
				if (!style) throw std::runtime_error("Style not found: " + name);
				ApplyStyleProps(style, args);
				style->UpdateData();
				ctx->ass->Commit("MCP: update style", AssFile::COMMIT_STYLES);
				return {{"name", name}, {"updated", true}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 3: lines — Subtitle line CRUD & batch operations
// ============================================================

static ToolDef MakeLinesTool() {
	return {
		"lines",
		"Subtitle line operations.\n"
		"Actions:\n"
		"- get: Get lines with optional pagination (start, count) and filtering (filter_style, filter_actor)\n"
		"- insert: Insert new lines (lines array required, position optional)\n"
		"- update: Batch update lines (updates array with index + fields to modify)\n"
		"- delete: Delete lines by indices\n"
		"- merge: Merge multiple lines into one (text concatenated with \\N)\n"
		"- split: Split a line at a time point\n"
		"- sort: Sort lines by field (start_time, end_time, style, actor, effect, layer)\n"
		"- find_replace: Find and replace text across lines",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get", "insert", "update", "delete", "merge", "split", "sort", "find_replace"}},
						{"description", "Operation to perform"}}},
			{"start", {{"type", "integer"}, {"description", "Start index for get (0-based)"}}},
			{"count", {{"type", "integer"}, {"description", "Number of lines for get"}}},
			{"filter_style", {{"type", "string"}, {"description", "Filter by style (for get)"}}},
			{"filter_actor", {{"type", "string"}, {"description", "Filter by actor (for get)"}}},
			{"lines", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "Lines to insert (for insert)"}}},
			{"position", {{"type", "integer"}, {"description", "Insert position (for insert)"}}},
			{"updates", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "Update objects with index + fields (for update)"}}},
			{"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Line indices (for delete/merge)"}}},
			{"index", {{"type", "integer"}, {"description", "Line index (for split)"}}},
			{"split_time", {{"type", "integer"}, {"description", "Split time in ms (for split)"}}},
			{"first_text", {{"type", "string"}, {"description", "Text for first part (for split)"}}},
			{"second_text", {{"type", "string"}, {"description", "Text for second part (for split)"}}},
			{"field", {{"type", "string"}, {"description", "Sort field or search field"}}},
			{"selection_only", {{"type", "boolean"}, {"description", "Only affect selected lines"}}},
			{"find", {{"type", "string"}, {"description", "Text to find (for find_replace)"}}},
			{"replace", {{"type", "string"}, {"description", "Replacement text (for find_replace)"}}},
			{"use_regex", {{"type", "boolean"}, {"description", "Use regex (for find_replace)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];

			if (action == "get") {
				int start = args.value("start", 0);
				int count = args.value("count", -1);
				std::string filter_style = args.value("filter_style", "");
				std::string filter_actor = args.value("filter_actor", "");
				json lines = json::array();
				int index = 0, returned = 0;
				for (auto& line : ctx->ass->Events) {
					if (index < start) { ++index; continue; }
					if (count >= 0 && returned >= count) break;
					if (!filter_style.empty() && line.Style.get() != filter_style) { ++index; continue; }
					if (!filter_actor.empty() && line.Actor.get() != filter_actor) { ++index; continue; }
					lines.push_back(DialogueToJson(line, index));
					++returned; ++index;
				}
				return {{"lines", lines}, {"total", CountLines(ctx)}};
			}
			else if (action == "insert") {
				auto& lines_arr = args.at("lines");
				int position = args.value("position", -1);
				auto insert_it = ctx->ass->Events.end();
				if (position >= 0) {
					insert_it = ctx->ass->Events.begin();
					for (int i = 0; i < position && insert_it != ctx->ass->Events.end(); ++i)
						++insert_it;
				}
				int inserted = 0;
				for (auto& lj : lines_arr) {
					auto* nl = new AssDialogue;
					nl->Text = lj.value("text", "");
					nl->Start = lj.value("start_time", 0);
					nl->End = lj.value("end_time", 5000);
					if (lj.contains("style")) nl->Style = lj["style"].get<std::string>();
					if (lj.contains("actor")) nl->Actor = lj["actor"].get<std::string>();
					if (lj.contains("effect")) nl->Effect = lj["effect"].get<std::string>();
					if (lj.contains("comment")) nl->Comment = lj["comment"].get<bool>();
					if (lj.contains("layer")) nl->Layer = lj["layer"].get<int>();
					if (lj.contains("margin_l")) nl->Margin[0] = lj["margin_l"].get<int>();
					if (lj.contains("margin_r")) nl->Margin[1] = lj["margin_r"].get<int>();
					if (lj.contains("margin_t")) nl->Margin[2] = lj["margin_t"].get<int>();
					ctx->ass->Events.insert(insert_it, *nl);
					++inserted;
				}
				ctx->ass->Commit("MCP: insert lines", AssFile::COMMIT_DIAG_ADDREM);
				return {{"inserted", inserted}};
			}
			else if (action == "update") {
				auto& updates = args.at("updates");
				int commit_type = 0, updated = 0;
				AssDialogue* last_line = nullptr;
				for (auto& upd : updates) {
					int index = upd["index"];
					auto* line = GetLineByIndex(ctx, index);
					if (!line) continue;
					if (upd.contains("text"))       { line->Text = upd["text"].get<std::string>(); commit_type |= AssFile::COMMIT_DIAG_TEXT; }
					if (upd.contains("start_time")) { line->Start = upd["start_time"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_TIME; }
					if (upd.contains("end_time"))   { line->End = upd["end_time"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_TIME; }
					if (upd.contains("style"))      { line->Style = upd["style"].get<std::string>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("actor"))      { line->Actor = upd["actor"].get<std::string>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("effect"))     { line->Effect = upd["effect"].get<std::string>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("comment"))    { line->Comment = upd["comment"].get<bool>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("layer"))      { line->Layer = upd["layer"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("margin_l"))   { line->Margin[0] = upd["margin_l"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("margin_r"))   { line->Margin[1] = upd["margin_r"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					if (upd.contains("margin_t"))   { line->Margin[2] = upd["margin_t"].get<int>(); commit_type |= AssFile::COMMIT_DIAG_META; }
					last_line = line;
					++updated;
				}
				if (commit_type)
					ctx->ass->Commit("MCP: batch update", commit_type, -1, updated == 1 ? last_line : nullptr);
				return {{"updated", updated}};
			}
			else if (action == "delete") {
				auto indices = args.at("indices").get<std::vector<int>>();
				std::set<AssDialogue*> to_delete_set;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (line) to_delete_set.insert(line);
				}
				if (to_delete_set.empty()) return {{"deleted", 0}};
				AssDialogue *pre_sel = nullptr, *post_sel = nullptr;
				bool hit_deletion = false;
				for (auto& diag : ctx->ass->Events) {
					if (to_delete_set.count(&diag))
						hit_deletion = true;
					else if (hit_deletion && !post_sel) {
						post_sel = &diag;
						break;
					} else
						pre_sel = &diag;
				}
				std::vector<std::unique_ptr<AssDialogue>> deferred;
				ctx->ass->Events.remove_and_dispose_if(
					[&](AssDialogue const& e) { return to_delete_set.count(const_cast<AssDialogue*>(&e)); },
					[&](AssDialogue* e) { deferred.emplace_back(e); });
				AssDialogue* new_active = post_sel ? post_sel : pre_sel;
				if (!new_active) {
					new_active = new AssDialogue;
					ctx->ass->Events.push_back(*new_active);
				}
				ctx->ass->Commit("MCP: delete lines", AssFile::COMMIT_DIAG_ADDREM);
				ctx->selectionController->SetSelectionAndActive({new_active}, new_active);
				return {{"deleted", (int)deferred.size()}};
			}
			else if (action == "merge") {
				auto indices = args.at("indices").get<std::vector<int>>();
				if (indices.size() < 2) throw std::runtime_error("Need at least 2 lines to merge");
				std::sort(indices.begin(), indices.end());
				std::vector<AssDialogue*> lines;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) throw std::runtime_error("Line index out of range: " + std::to_string(idx));
					lines.push_back(line);
				}
				auto* first = lines[0];
				std::string merged_text = first->Text.get();
				int min_start = (int)first->Start, max_end = (int)first->End;
				for (size_t i = 1; i < lines.size(); ++i) {
					merged_text += "\\N" + lines[i]->Text.get();
					min_start = std::min(min_start, (int)lines[i]->Start);
					max_end = std::max(max_end, (int)lines[i]->End);
				}
				first->Text = merged_text;
				first->Start = min_start;
				first->End = max_end;
				for (size_t i = lines.size() - 1; i >= 1; --i) {
					ctx->ass->Events.erase(ctx->ass->Events.iterator_to(*lines[i]));
					delete lines[i];
				}
				ctx->ass->Commit("MCP: merge lines", AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_TIME);
				return {{"merged_into_index", indices[0]}};
			}
			else if (action == "split") {
				int idx = args.at("index");
				int split_time = args.at("split_time");
				auto* line = GetLineByIndex(ctx, idx);
				if (!line) throw std::runtime_error("Line index out of range");
				int start = (int)line->Start, end = (int)line->End;
				if (split_time <= start || split_time >= end)
					throw std::runtime_error("split_time must be between line start and end time");
				auto* new_line = new AssDialogue(*line);
				line->End = split_time;
				new_line->Start = split_time;
				if (args.contains("first_text"))
					line->Text = args["first_text"].get<std::string>();
				if (args.contains("second_text"))
					new_line->Text = args["second_text"].get<std::string>();
				else if (!args.contains("first_text"))
					new_line->Text = "";
				auto it = ctx->ass->Events.iterator_to(*line);
				ctx->ass->Events.insert(++it, *new_line);
				ctx->ass->Commit("MCP: split line", AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME);
				return {{"first_index", idx}, {"second_index", idx + 1}, {"first_end", split_time}, {"second_start", split_time}};
			}
			else if (action == "sort") {
				std::string field = args.at("field");
				bool sel_only = args.value("selection_only", false);
				AssFile::CompFunc comp;
				if (field == "start_time") comp = AssFile::CompStart;
				else if (field == "end_time") comp = AssFile::CompEnd;
				else if (field == "style") comp = AssFile::CompStyle;
				else if (field == "actor") comp = AssFile::CompActor;
				else if (field == "effect") comp = AssFile::CompEffect;
				else if (field == "layer") comp = AssFile::CompLayer;
				else throw std::runtime_error("Unknown sort field: " + field);
				std::set<AssDialogue*> limit;
				if (sel_only) {
					auto& sel = ctx->selectionController->GetSelectedSet();
					limit.insert(sel.begin(), sel.end());
				}
				ctx->ass->Sort(comp, limit);
				ctx->ass->Commit("MCP: sort lines", AssFile::COMMIT_ORDER);
				return {{"sorted", true}, {"field", field}};
			}
			else if (action == "find_replace") {
				std::string find_str = args.at("find");
				std::string replace_str = args.at("replace");
				int replacements = 0, commit_type = 0;
				for (auto& line : ctx->ass->Events) {
					std::string text = line.Text.get();
					size_t pos = 0;
					bool changed = false;
					while ((pos = text.find(find_str, pos)) != std::string::npos) {
						text.replace(pos, find_str.length(), replace_str);
						pos += replace_str.length();
						++replacements;
						changed = true;
					}
					if (changed) {
						line.Text = text;
						commit_type |= AssFile::COMMIT_DIAG_TEXT;
					}
				}
				if (commit_type)
					ctx->ass->Commit("MCP: find/replace", commit_type);
				return {{"replacements", replacements}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 4: timing — Timeline operations
// ============================================================

static ToolDef MakeTimingTool() {
	return {
		"timing",
		"Timeline & timing operations.\n"
		"Actions:\n"
		"- shift: Shift start/end times by offset_ms\n"
		"- snap_to_keyframe: Snap times to nearest keyframe\n"
		"- make_continuous: Remove gaps between adjacent lines\n"
		"- add_lead_in_out: Extend start earlier and/or end later\n"
		"- generate_from_text: Create timed lines from text array",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"},
				{"enum", {"shift", "snap_to_keyframe", "make_continuous", "add_lead_in_out", "generate_from_text"}},
				{"description", "Operation to perform"}}},
			{"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
			{"offset_ms", {{"type", "integer"}}},
			{"target", {{"type", "string"}, {"description", "start/end/both"}}},
			{"direction", {{"type", "string"}, {"enum", {"prev", "next", "nearest"}}}},
			{"lead_in_ms", {{"type", "integer"}}},
			{"lead_out_ms", {{"type", "integer"}}},
			{"lines", {{"type", "array"}, {"items", {{"type", "object"}}}}},
			{"start_ms", {{"type", "integer"}}},
			{"end_ms", {{"type", "integer"}}},
			{"gap_ms", {{"type", "integer"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "shift") {
				auto indices = args.at("indices").get<std::vector<int>>();
				int offset = args.at("offset_ms");
				int shifted = 0;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) continue;
					line->Start = std::max(0, (int)line->Start + offset);
					line->End = std::max(0, (int)line->End + offset);
					++shifted;
				}
				if (shifted > 0)
					ctx->ass->Commit("MCP: shift times", AssFile::COMMIT_DIAG_TIME);
				return {{"shifted", shifted}};
			}
			else if (action == "snap_to_keyframe") {
				auto indices = args.at("indices").get<std::vector<int>>();
				std::string target = args.at("target");
				std::string direction = args.at("direction");
				auto* vc = ctx->videoController.get();
				if (!vc) throw std::runtime_error("No video loaded");
				auto keyframes = ctx->project->Keyframes();
				if (keyframes.empty()) throw std::runtime_error("No keyframes");
				std::vector<int> kf_times;
				for (int kf : keyframes)
					kf_times.push_back(vc->TimeAtFrame(kf));
				std::sort(kf_times.begin(), kf_times.end());
				auto snap = [&](int t) -> int {
					if (direction == "prev") {
						auto it = std::upper_bound(kf_times.begin(), kf_times.end(), t);
						return it == kf_times.begin() ? kf_times.front() : *std::prev(it);
					} else if (direction == "next") {
						auto it = std::lower_bound(kf_times.begin(), kf_times.end(), t);
						return it == kf_times.end() ? kf_times.back() : *it;
					} else {
						auto it = std::lower_bound(kf_times.begin(), kf_times.end(), t);
						if (it == kf_times.end()) return kf_times.back();
						if (it == kf_times.begin()) return *it;
						auto prev = std::prev(it);
						return (t - *prev <= *it - t) ? *prev : *it;
					}
				};
				int snapped = 0;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) continue;
					if (target == "start" || target == "both") line->Start = snap((int)line->Start);
					if (target == "end" || target == "both") line->End = snap((int)line->End);
					++snapped;
				}
				if (snapped > 0)
					ctx->ass->Commit("MCP: snap to keyframe", AssFile::COMMIT_DIAG_TIME);
				return {{"snapped", snapped}};
			}
			else if (action == "make_continuous") {
				auto indices = args.at("indices").get<std::vector<int>>();
				std::string target = args.at("target");
				if (indices.size() < 2) throw std::runtime_error("Need at least 2 lines");
				std::sort(indices.begin(), indices.end());
				std::vector<AssDialogue*> lines;
				for (int idx : indices) {
					auto* l = GetLineByIndex(ctx, idx);
					if (!l) throw std::runtime_error("Index out of range: " + std::to_string(idx));
					lines.push_back(l);
				}
				int adjusted = 0;
				if (target == "start") {
					for (size_t i = 1; i < lines.size(); ++i) { lines[i]->Start = lines[i-1]->End; ++adjusted; }
				} else {
					for (size_t i = 0; i+1 < lines.size(); ++i) { lines[i]->End = lines[i+1]->Start; ++adjusted; }
				}
				if (adjusted > 0)
					ctx->ass->Commit("MCP: make continuous", AssFile::COMMIT_DIAG_TIME);
				return {{"adjusted", adjusted}};
			}
			else if (action == "add_lead_in_out") {
				auto indices = args.at("indices").get<std::vector<int>>();
				int lead_in = args.value("lead_in_ms", 0);
				int lead_out = args.value("lead_out_ms", 0);
				int adjusted = 0;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) continue;
					if (lead_in > 0) line->Start = std::max(0, (int)line->Start - lead_in);
					if (lead_out > 0) line->End = (int)line->End + lead_out;
					++adjusted;
				}
				if (adjusted > 0)
					ctx->ass->Commit("MCP: add lead in/out", AssFile::COMMIT_DIAG_TIME);
				return {{"adjusted", adjusted}};
			}
			else if (action == "generate_from_text") {
				auto lines_arr = args.at("lines");
				int start_ms = args.at("start_ms");
				int end_ms = args.at("end_ms");
				int gap_ms = args.value("gap_ms", 0);
				if (lines_arr.empty()) throw std::runtime_error("lines array is empty");
				if (end_ms <= start_ms) throw std::runtime_error("end_ms must be > start_ms");
				int n = (int)lines_arr.size();
				int total_gap = gap_ms * (n - 1);
				int total_dur = end_ms - start_ms - total_gap;
				if (total_dur <= 0) throw std::runtime_error("Not enough time");
				std::vector<int> lengths;
				int total_len = 0;
				for (auto& l : lines_arr) {
					int len = std::max(1, (int)l["text"].get<std::string>().size());
					lengths.push_back(len);
					total_len += len;
				}
				int cur = start_ms, created = 0;
				for (int i = 0; i < n; ++i) {
					int dur = (i == n-1) ? (end_ms - cur) : (total_dur * lengths[i] / total_len);
					auto* d = new AssDialogue;
					d->Start = cur; d->End = cur + dur;
					d->Text = lines_arr[i]["text"].get<std::string>();
					if (lines_arr[i].contains("style")) d->Style = lines_arr[i]["style"].get<std::string>();
					if (lines_arr[i].contains("actor")) d->Actor = lines_arr[i]["actor"].get<std::string>();
					ctx->ass->Events.push_back(*d);
					cur += dur + gap_ms;
					++created;
				}
				ctx->ass->Commit("MCP: generate timing", AssFile::COMMIT_DIAG_ADDREM);
				return {{"created", created}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 5: selection — Selection management
// ============================================================

static ToolDef MakeSelectionTool() {
	return {
		"selection",
		"Selection management.\n"
		"Actions:\n"
		"- get: Get selected line indices and active line\n"
		"- set: Set selection and optionally active line",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get", "set"}}, {"description", "Operation to perform"}}},
			{"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Line indices to select (for set)"}}},
			{"active", {{"type", "integer"}, {"description", "Active line index (for set)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "get") {
				auto& sel = ctx->selectionController->GetSelectedSet();
				auto* active = ctx->selectionController->GetActiveLine();
				std::unordered_map<AssDialogue*, int> index_map;
				int i = 0;
				for (auto& line : ctx->ass->Events) index_map[&line] = i++;
				json indices = json::array();
				for (auto* line : sel) {
					auto it = index_map.find(line);
					if (it != index_map.end()) indices.push_back(it->second);
				}
				std::sort(indices.begin(), indices.end());
				int active_index = -1;
				if (active) {
					auto it = index_map.find(active);
					if (it != index_map.end()) active_index = it->second;
				}
				return {{"selected_indices", indices}, {"active_index", active_index}};
			}
			else if (action == "set") {
				auto indices = args.at("indices").get<std::vector<int>>();
				int active_idx = args.value("active", indices.empty() ? -1 : indices[0]);
				Selection new_sel;
				AssDialogue* active_line = nullptr;
				int i = 0;
				for (auto& line : ctx->ass->Events) {
					if (std::find(indices.begin(), indices.end(), i) != indices.end())
						new_sel.insert(&line);
					if (i == active_idx) active_line = &line;
					++i;
				}
				ctx->selectionController->SetSelectionAndActive(std::move(new_sel), active_line);
				return {{"selected", (int)indices.size()}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 6: audio — Audio operations
// ============================================================

static ToolDef MakeAudioTool() {
	return {
		"audio",
		"Audio operations.\n"
		"Actions:\n"
		"- get_peaks: Get audio peak levels for a time range (returns 0.0-1.0 values)\n"
		"- get_segment: Export audio segment as base64 WAV (max 30s)",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get_peaks", "get_segment"}}, {"description", "Operation to perform"}}},
			{"start_ms", {{"type", "integer"}, {"description", "Start time in ms"}}},
			{"end_ms", {{"type", "integer"}, {"description", "End time in ms"}}},
			{"num_peaks", {{"type", "integer"}, {"description", "Number of peak values (for get_peaks, default: 100)"}}}
		}}, {"required", json::array({"action", "start_ms", "end_ms"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			int start_ms = args["start_ms"];
			int end_ms = args["end_ms"];
			if (start_ms >= end_ms) throw std::runtime_error("start_ms must be < end_ms");
			auto* provider = ctx->project->AudioProvider();
			if (!provider) throw std::runtime_error("No audio loaded");
			int sample_rate = provider->GetSampleRate();
			int channels = provider->GetChannels();
			int64_t max_samples = provider->GetNumSamples();

			if (action == "get_peaks") {
				int num_peaks = args.value("num_peaks", 100);
				if (num_peaks < 1 || num_peaks > 10000) throw std::runtime_error("num_peaks must be 1-10000");
				int64_t start_sample = std::min(max_samples, (int64_t)start_ms * sample_rate / 1000);
				int64_t end_sample = std::min(max_samples, (int64_t)end_ms * sample_rate / 1000);
				int64_t total_samples = end_sample - start_sample;
				if (total_samples <= 0) throw std::runtime_error("No audio samples in range");
				int64_t spp = std::max((int64_t)1, total_samples / num_peaks);
				json peaks = json::array();
				std::vector<int16_t> buf(spp * channels);
				for (int i = 0; i < num_peaks; ++i) {
					int64_t cs = start_sample + i * spp;
					int64_t cc = std::min(spp, end_sample - cs);
					if (cc <= 0) break;
					provider->GetAudio(buf.data(), cs, cc);
					int16_t peak = 0;
					for (int64_t s = 0; s < cc * channels; ++s) {
						int16_t v = buf[s] < 0 ? -buf[s] : buf[s];
						if (v > peak) peak = v;
					}
					peaks.push_back(std::round(peak / 32768.0 * 1000.0) / 1000.0);
				}
				return {{"peaks", peaks}, {"sample_rate", sample_rate}, {"channels", channels},
						{"duration_ms", end_ms - start_ms}, {"peak_count", (int)peaks.size()}};
			}
			else if (action == "get_segment") {
				if (end_ms - start_ms > 30000) throw std::runtime_error("Maximum duration is 30 seconds");
				int bps = provider->GetBytesPerSample();
				int64_t start_sample = std::min(max_samples, ((int64_t)start_ms * sample_rate + 999) / 1000);
				int64_t end_sample = std::min(max_samples, ((int64_t)end_ms * sample_rate + 999) / 1000);
				int64_t num_samples = end_sample - start_sample;
				if (num_samples <= 0) throw std::runtime_error("No audio samples in range");
				size_t bpf = bps * channels;
				size_t data_size = num_samples * bpf;
				size_t wav_size = 44 + data_size;
				std::vector<uint8_t> wav(wav_size);
				auto w16 = [&](size_t o, int16_t v) { memcpy(&wav[o], &v, 2); };
				auto w32 = [&](size_t o, int32_t v) { memcpy(&wav[o], &v, 4); };
				memcpy(&wav[0], "RIFF", 4); w32(4, (int32_t)(wav_size - 8));
				memcpy(&wav[8], "WAVE", 4);
				memcpy(&wav[12], "fmt ", 4); w32(16, 16);
				w16(20, 1); w16(22, (int16_t)channels);
				w32(24, sample_rate); w32(28, sample_rate * channels * bps);
				w16(32, (int16_t)(channels * bps)); w16(34, (int16_t)(bps * 8));
				memcpy(&wav[36], "data", 4); w32(40, (int32_t)data_size);
				const size_t spr = 65536 / bpf;
				for (int64_t i = start_sample; i < end_sample; ) {
					int64_t count = std::min((int64_t)spr, end_sample - i);
					provider->GetAudio(&wav[44 + (i - start_sample) * bpf], i, count);
					i += count;
				}
				return {{"data", Base64Encode(wav.data(), wav.size())}, {"format", "wav"},
						{"sample_rate", sample_rate}, {"channels", channels},
						{"bits_per_sample", bps * 8}, {"duration_ms", end_ms - start_ms}, {"size_bytes", (int)wav_size}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 7: tags — ASS tags & karaoke
// ============================================================

static ToolDef MakeTagsTool() {
	return {
		"tags",
		"ASS override tags & karaoke operations.\n"
		"Actions:\n"
		"- parse: Parse ASS override tags from a line into structured data\n"
		"- strip: Remove all ASS tags from lines, leaving plain text\n"
		"- parse_karaoke: Parse karaoke syllable timing from a line\n"
		"- set_karaoke: Set karaoke timing on a line",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"parse", "strip", "parse_karaoke", "set_karaoke"}},
						{"description", "Operation to perform"}}},
			{"index", {{"type", "integer"}, {"description", "Line index (for parse/parse_karaoke/set_karaoke)"}}},
			{"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Line indices (for strip)"}}},
			{"syllables", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "Karaoke syllables [{duration, text}] (for set_karaoke)"}}},
			{"tag_type", {{"type", "string"}, {"enum", {"k", "kf", "ko"}}, {"description", "Karaoke tag type (for set_karaoke, default: k)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "parse") {
				int idx = args.at("index");
				auto* line = GetLineByIndex(ctx, idx);
				if (!line) throw std::runtime_error("Line index out of range");
				auto blocks = line->ParseTags();
				json result = json::array();
				for (auto& block : blocks) {
					auto type = block->GetType();
					if (type == AssBlockType::PLAIN)
						result.push_back({{"type", "plain"}, {"text", block->GetText()}});
					else if (type == AssBlockType::DRAWING)
						result.push_back({{"type", "drawing"}, {"text", block->GetText()}});
					else if (type == AssBlockType::COMMENT)
						result.push_back({{"type", "comment"}, {"text", block->GetText()}});
					else if (type == AssBlockType::OVERRIDE) {
						auto* ovr = static_cast<AssDialogueBlockOverride*>(block.get());
						ovr->ParseTags();
						json tags = json::array();
						for (auto& tag : ovr->Tags) {
							if (!tag.IsValid()) continue;
							json t = {{"name", tag.Name}};
							json params = json::array();
							for (auto& p : tag.Params) {
								if (p.omitted) continue;
								json pj;
								switch (p.GetType()) {
									case VariableDataType::INT: pj = p.Get<int>(0); break;
									case VariableDataType::FLOAT: pj = p.Get<double>(0.0); break;
									case VariableDataType::BOOL: pj = p.Get<bool>(false); break;
									default: pj = p.Get<std::string>(""); break;
								}
								params.push_back(pj);
							}
							t["params"] = params;
							tags.push_back(t);
						}
						result.push_back({{"type", "override"}, {"tags", tags}});
					}
				}
				return {{"blocks", result}};
			}
			else if (action == "strip") {
				auto indices = args.at("indices").get<std::vector<int>>();
				int stripped = 0;
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) continue;
					line->StripTags();
					++stripped;
				}
				if (stripped > 0)
					ctx->ass->Commit("MCP: strip tags", AssFile::COMMIT_DIAG_TEXT);
				return {{"stripped", stripped}};
			}
			else if (action == "parse_karaoke") {
				int idx = args.at("index");
				auto* line = GetLineByIndex(ctx, idx);
				if (!line) throw std::runtime_error("Line index out of range");
				auto syls = ParseKaraokeSyllables(line);
				json syllables = json::array();
				for (auto& syl : syls)
					syllables.push_back({{"start_time", syl.start_time}, {"duration", syl.duration},
										 {"text", syl.text}, {"tag_type", syl.tag_type}});
				return {{"syllables", syllables}, {"count", (int)syls.size()}};
			}
			else if (action == "set_karaoke") {
				int idx = args.at("index");
				auto* line = GetLineByIndex(ctx, idx);
				if (!line) throw std::runtime_error("Line index out of range");
				std::string tag = "\\" + args.value("tag_type", std::string("k"));
				std::string new_text;
				for (auto& syl : args.at("syllables")) {
					int dur = syl["duration"];
					std::string text = syl["text"];
					new_text += "{" + tag + std::to_string(dur) + "}" + text;
				}
				line->Text = new_text;
				ctx->ass->Commit("MCP: set karaoke", AssFile::COMMIT_DIAG_TEXT);
				return {{"index", idx}, {"text", new_text}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 8: text_analysis — Text analysis & quality checks
// ============================================================

static ToolDef MakeTextAnalysisTool() {
	return {
		"text_analysis",
		"Text analysis & quality checks.\n"
		"Actions:\n"
		"- get_extents: Calculate rendered text size in a given style\n"
		"- get_line_length: Get character count and max line length\n"
		"- validate: Run quality checks (overlap, duration, line_length, gap)",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get_extents", "get_line_length", "validate"}},
						{"description", "Operation to perform"}}},
			{"text", {{"type", "string"}, {"description", "Text to measure (for get_extents)"}}},
			{"style", {{"type", "string"}, {"description", "Style name (for get_extents)"}}},
			{"indices", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Line indices (for get_line_length)"}}},
			{"ignore_whitespace", {{"type", "boolean"}}},
			{"ignore_punctuation", {{"type", "boolean"}}},
			{"checks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Checks to run (for validate): overlap, duration, line_length, gap"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "get_extents") {
				std::string text = args.at("text");
				std::string style_name = args.at("style");
				auto* style = FindStyleByName(ctx, style_name);
				if (!style) throw std::runtime_error("Style not found: " + style_name);
				double width, height, descent, extlead;
				if (!Automation4::CalculateTextExtents(style, text, width, height, descent, extlead))
					throw std::runtime_error("Failed to calculate text extents");
				return {{"width", width}, {"height", height}, {"descent", descent}, {"external_leading", extlead}};
			}
			else if (action == "get_line_length") {
				auto indices = args.at("indices").get<std::vector<int>>();
				int mask = agi::IGNORE_BLOCKS;
				if (args.value("ignore_whitespace", false)) mask |= agi::IGNORE_WHITESPACE;
				if (args.value("ignore_punctuation", false)) mask |= agi::IGNORE_PUNCTUATION;
				json results = json::array();
				for (int idx : indices) {
					auto* line = GetLineByIndex(ctx, idx);
					if (!line) continue;
					std::string text = line->Text.get();
					results.push_back({{"index", idx},
						{"max_line_length", (int)agi::MaxLineLength(text, mask)},
						{"character_count", (int)agi::CharacterCount(text, mask)}});
				}
				return {{"results", results}};
			}
			else if (action == "validate") {
				auto checks = args.value("checks", std::vector<std::string>{"overlap", "duration", "line_length", "gap"});
				bool ck_overlap = std::find(checks.begin(), checks.end(), "overlap") != checks.end();
				bool ck_duration = std::find(checks.begin(), checks.end(), "duration") != checks.end();
				bool ck_length = std::find(checks.begin(), checks.end(), "line_length") != checks.end();
				bool ck_gap = std::find(checks.begin(), checks.end(), "gap") != checks.end();
				json issues = json::array();
				struct LI { AssDialogue* line; int index; };
				std::vector<LI> lines;
				int idx = 0;
				for (auto& line : ctx->ass->Events) {
					if (!line.Comment) lines.push_back({&line, idx});
					++idx;
				}
				std::sort(lines.begin(), lines.end(), [](auto& a, auto& b) { return (int)a.line->Start < (int)b.line->Start; });
				for (size_t i = 0; i < lines.size(); ++i) {
					auto& li = lines[i];
					int start = (int)li.line->Start, end = (int)li.line->End, dur = end - start;
					if (ck_duration && dur < 500)
						issues.push_back({{"index", li.index}, {"type", "short_duration"},
							{"message", "Duration is " + std::to_string(dur) + "ms (< 500ms)"}});
					if (ck_duration && dur > 10000)
						issues.push_back({{"index", li.index}, {"type", "long_duration"},
							{"message", "Duration is " + std::to_string(dur) + "ms (> 10s)"}});
					if (ck_length) {
						auto stripped = li.line->GetStrippedText();
						if (stripped.length() > 80)
							issues.push_back({{"index", li.index}, {"type", "long_line"},
								{"message", "Line has " + std::to_string(stripped.length()) + " characters (> 80)"}});
					}
					if (i + 1 < lines.size()) {
						int ns = (int)lines[i+1].line->Start;
						if (ck_overlap && end > ns)
							issues.push_back({{"index", li.index}, {"type", "overlap"},
								{"message", "Overlaps with line " + std::to_string(lines[i+1].index) + " by " + std::to_string(end - ns) + "ms"}});
						if (ck_gap && ns - end > 0 && ns - end < 100)
							issues.push_back({{"index", li.index}, {"type", "small_gap"},
								{"message", "Gap of " + std::to_string(ns - end) + "ms before line " + std::to_string(lines[i+1].index)}});
					}
				}
				return {{"issues", issues}, {"issue_count", (int)issues.size()}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 9: cleanup — Subtitle cleanup operations
// ============================================================

static ToolDef MakeCleanupTool() {
	return {
		"cleanup",
		"Subtitle cleanup operations.\n"
		"Actions:\n"
		"- recombine_overlaps: Split overlapping lines into non-overlapping segments\n"
		"- merge_identical: Merge sequential lines with identical text",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"recombine_overlaps", "merge_identical"}},
						{"description", "Operation to perform"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "recombine_overlaps") {
				int before = CountLines(ctx);
				ctx->ass->Sort(AssFile::CompStart);
				auto& first = ctx->ass->Events.front();
				ctx->selectionController->SetSelectionAndActive({&first}, &first);
				SubtitleFormat::RecombineOverlaps(*ctx->ass);
				auto& nf = ctx->ass->Events.front();
				ctx->selectionController->SetSelectionAndActive({&nf}, &nf);
				ctx->ass->Commit("MCP: recombine overlaps",
					AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_ORDER);
				return {{"recombined", true}, {"lines_before", before}, {"lines_after", CountLines(ctx)}};
			}
			else if (action == "merge_identical") {
				int before = CountLines(ctx);
				auto& first = ctx->ass->Events.front();
				ctx->selectionController->SetSelectionAndActive({&first}, &first);
				SubtitleFormat::MergeIdentical(*ctx->ass);
				auto& nf = ctx->ass->Events.front();
				ctx->selectionController->SetSelectionAndActive({&nf}, &nf);
				ctx->ass->Commit("MCP: merge identical", AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME);
				return {{"merged", true}, {"lines_before", before}, {"lines_after", CountLines(ctx)}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 10: file — File operations
// ============================================================

static ToolDef MakeFileTool() {
	return {
		"file",
		"File operations.\n"
		"Actions:\n"
		"- save: Save subtitle file (optional path for Save As)\n"
		"- export_ass: Get raw ASS text content (does not write to disk)\n"
		"- export: Export to another format (.srt, .ssa, .txt etc.)\n"
		"- undo: Undo the last operation",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"save", "export_ass", "export", "undo"}},
						{"description", "Operation to perform"}}},
			{"path", {{"type", "string"}, {"description", "File path (for save/export)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "save") {
				std::string path = args.value("path", "");
				if (path.empty()) {
					auto current = ctx->subsController->Filename();
					ctx->subsController->Save(current);
				} else {
					ctx->subsController->Save(agi::fs::path(path));
				}
				return {{"saved", true}, {"path", ctx->subsController->Filename().string()}};
			}
			else if (action == "export_ass") {
				std::ostringstream oss;
				oss << "[Script Info]\n";
				for (auto& info : ctx->ass->Info)
					oss << info.Key() << ": " << info.Value() << "\n";
				oss << "\n[V4+ Styles]\n";
				oss << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
				for (auto& style : ctx->ass->Styles)
					oss << style.GetEntryData() << "\n";
				oss << "\n[Events]\n";
				oss << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
				for (auto& line : ctx->ass->Events)
					oss << line.GetEntryData() << "\n";
				return {{"content", json::array({{{"type", "text"}, {"text", oss.str()}}})}};
			}
			else if (action == "export") {
				std::string path = args.at("path");
				agi::fs::path fpath(path);
				auto* writer = SubtitleFormat::GetWriter(fpath);
				if (!writer) throw std::runtime_error("No subtitle format writer for: " + path);
				agi::vfr::Framerate fps;
				auto* vp = ctx->project->VideoProvider();
				if (vp) fps = vp->GetFPS();
				else fps = agi::vfr::Framerate(24000, 1001);
				writer->WriteFile(ctx->ass.get(), fpath, fps, "");
				return {{"exported", true}, {"path", path}};
			}
			else if (action == "undo") {
				if (ctx->subsController->IsUndoStackEmpty())
					throw std::runtime_error("Nothing to undo");
				cmd::call("edit/undo", ctx);
				return {{"undone", true}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Tool 11: video — Video operations
// ============================================================

static ToolDef MakeVideoTool() {
	return {
		"video",
		"Video operations.\n"
		"Actions:\n"
		"- get_frame: Get a video frame as base64 PNG (specify frame number or time_ms)\n"
		"- convert_time: Convert between frame number and time_ms (provide one)\n"
		"- get_keyframes: Get keyframe list with timestamps",
		{{"type", "object"}, {"properties", {
			{"action", {{"type", "string"}, {"enum", {"get_frame", "convert_time", "get_keyframes"}},
						{"description", "Operation to perform"}}},
			{"frame", {{"type", "integer"}, {"description", "Frame number"}}},
			{"time_ms", {{"type", "integer"}, {"description", "Time in milliseconds"}}},
			{"max_width", {{"type", "integer"}, {"description", "Max width for downscaling (for get_frame, default: 960)"}}}
		}}, {"required", json::array({"action"})}},
		[](const json& args, agi::Context* ctx) -> json {
			std::string action = args["action"];
			if (action == "get_frame") {
				auto* vp = ctx->project->VideoProvider();
				if (!vp) throw std::runtime_error("No video loaded");
				int frame;
				if (args.contains("frame")) frame = args["frame"];
				else if (args.contains("time_ms")) frame = ctx->videoController->FrameAtTime(args["time_ms"]);
				else frame = 0;
				int max_width = args.value("max_width", 960);
				auto vf = vp->GetFrame(frame, ctx->videoController->TimeAtFrame(frame), false);
				if (!vf) throw std::runtime_error("Failed to get video frame");
				wxImage img = GetImage(*vf);
				if (max_width > 0 && img.GetWidth() > max_width) {
					int nh = img.GetHeight() * max_width / img.GetWidth();
					img.Rescale(max_width, nh, wxIMAGE_QUALITY_HIGH);
				}
				wxMemoryOutputStream mos;
				if (!img.SaveFile(mos, wxBITMAP_TYPE_PNG))
					throw std::runtime_error("Failed to encode PNG");
				size_t size = mos.GetSize();
				std::vector<uint8_t> buf(size);
				mos.CopyTo(buf.data(), size);
				return {{"data", Base64Encode(buf.data(), buf.size())}, {"format", "png"},
						{"width", img.GetWidth()}, {"height", img.GetHeight()},
						{"frame", frame}, {"size_bytes", (int)size}};
			}
			else if (action == "convert_time") {
				auto* vc = ctx->videoController.get();
				if (!vc) throw std::runtime_error("No video loaded");
				bool has_frame = args.contains("frame");
				bool has_time = args.contains("time_ms");
				if (has_frame == has_time)
					throw std::runtime_error("Provide exactly one of 'frame' or 'time_ms'");
				if (has_frame)
					return {{"time_ms", vc->TimeAtFrame(args["frame"].get<int>())}};
				else
					return {{"frame", vc->FrameAtTime(args["time_ms"].get<int>())}};
			}
			else if (action == "get_keyframes") {
				auto keyframes = ctx->project->Keyframes();
				json result = json::array();
				auto* vc = ctx->videoController.get();
				for (int kf : keyframes) {
					json entry = {{"frame", kf}};
					if (vc) entry["time_ms"] = vc->TimeAtFrame(kf);
					result.push_back(entry);
				}
				return {{"keyframes", result}};
			}
			throw std::runtime_error("Unknown action: " + action);
		}
	};
}

// ============================================================
// Registration
// ============================================================

std::vector<ToolDef> RegisterAllTools() {
	return {
		MakeProjectTool(),
		MakeStylesTool(),
		MakeLinesTool(),
		MakeTimingTool(),
		MakeSelectionTool(),
		MakeAudioTool(),
		MakeTagsTool(),
		MakeTextAnalysisTool(),
		MakeCleanupTool(),
		MakeFileTool(),
		MakeVideoTool()
	};
}

} // namespace mcp
