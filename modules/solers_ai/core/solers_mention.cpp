/**************************************************************************/
/*  solers_mention.cpp                                                    */
/**************************************************************************/

#include "solers_mention.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/templates/hash_set.h"
#include "editor/editor_interface.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/plugins/solers_plugin.h"
#include "scene/main/node.h"

namespace SolersMention {

static bool _id_char(char32_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || (p_char >= '0' && p_char <= '9') || p_char == '-' || p_char == '_' || p_char == '.';
}

static bool _path_body_char(char32_t p_char) {
	return _id_char(p_char) || p_char == '/' || p_char == ':' || p_char == '%' || p_char == '+';
}

static String _source_of(const Dictionary &p_mention) {
	const String source = String(p_mention.get("source", "plugin")).strip_edges().to_lower();
	return source.is_empty() ? String("plugin") : source;
}

static bool _path_exists(const String &p_path) {
	const String path = p_path.strip_edges();
	if (path.is_empty()) {
		return false;
	}
	if (ResourceLoader::exists(path)) {
		return true;
	}
	return FileAccess::exists(path);
}

static bool _node_exists(const String &p_node_path) {
	String path = p_node_path.strip_edges();
	if (path.is_empty()) {
		return false;
	}
	while (path.begins_with("/")) {
		path = path.substr(1);
	}
	EditorInterface *editor = EditorInterface::get_singleton();
	Node *root = editor ? editor->get_edited_scene_root() : nullptr;
	if (!root) {
		return false;
	}
	if (root->get_node_or_null(NodePath(path)) != nullptr) {
		return true;
	}
	if (root->get_node_or_null(NodePath("/" + path)) != nullptr) {
		return true;
	}
	// Absolute tree paths from observation serialization.
	if (root->is_inside_tree() && root->get_node_or_null(NodePath(p_node_path.strip_edges())) != nullptr) {
		return true;
	}
	return false;
}

static Dictionary _plugin_mention(SolersPlugin *p_plugin) {
	Dictionary mention;
	const Dictionary profile = p_plugin->get_profile();
	const String id = String(profile.get("id", String())).strip_edges().to_lower();
	mention["source"] = "plugin";
	mention["id"] = id;
	mention["label"] = profile.get("label", id);
	mention["kinds"] = profile.get("kinds", Array());
	mention["path"] = id;
	return mention;
}

static Dictionary _path_mention(const String &p_source, const String &p_path, const String &p_label = String()) {
	Dictionary mention;
	const String path = p_path.strip_edges();
	mention["source"] = p_source;
	mention["id"] = path;
	mention["path"] = path;
	mention["label"] = p_label.is_empty() ? path.get_file() : p_label;
	return mention;
}

static bool _matches_query(const Dictionary &p_mention, const String &p_query) {
	const String query = p_query.strip_edges().to_lower();
	if (query.is_empty()) {
		return true;
	}
	const String id = String(p_mention.get("id", String())).to_lower();
	const String label = String(p_mention.get("label", String())).to_lower();
	const String path = String(p_mention.get("path", String())).to_lower();
	const String source = _source_of(p_mention);
	const String token = format_token(p_mention).to_lower();
	if (id.begins_with(query) || label.begins_with(query) || path.contains(query) || token.contains(query)) {
		return true;
	}
	// Allow filtering with typed prefixes: "file:foo", "mes", "scene:res"
	if (query.begins_with(source + ":")) {
		const String rest = query.substr(source.length() + 1);
		return rest.is_empty() || id.contains(rest) || path.contains(rest) || label.contains(rest);
	}
	return false;
}

static void _append_unique(Array &r_items, HashSet<String> &r_seen, const Dictionary &p_mention) {
	const String key = dedupe_key(p_mention);
	if (key.is_empty() || r_seen.has(key)) {
		return;
	}
	r_seen.insert(key);
	r_items.push_back(p_mention);
}

static Array _collect_plugins(const String &p_query) {
	Array items;
	HashSet<String> seen;
	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		Dictionary mention = _plugin_mention(plugin);
		if (!_matches_query(mention, p_query)) {
			continue;
		}
		_append_unique(items, seen, mention);
	}
	return items;
}

static Array _collect_files(SolersObservationService *p_observation, const String &p_query) {
	Array items;
	if (!p_observation) {
		return items;
	}
	HashSet<String> seen;
	Dictionary args;
	args["type"] = "path";
	args["query"] = p_query;
	args["max_results"] = COLLECT_LIMIT;
	const Dictionary search = p_observation->search_project(args);
	const Array results = search.get("results", Array());
	for (int i = 0; i < results.size() && items.size() < COLLECT_LIMIT; i++) {
		const String path = String(Dictionary(results[i]).get("path", String())).strip_edges();
		if (path.is_empty() || !_path_exists(path)) {
			continue;
		}
		Dictionary mention = _path_mention("file", path);
		if (!_matches_query(mention, p_query)) {
			continue;
		}
		_append_unique(items, seen, mention);
	}
	return items;
}

static Array _collect_scenes(SolersObservationService *p_observation, const String &p_query) {
	Array items;
	HashSet<String> seen;
	if (p_observation) {
		const Dictionary open = p_observation->get_open_scenes(0, 0);
		const Array paths = open.get("paths", Array());
		for (int i = 0; i < paths.size() && items.size() < COLLECT_LIMIT; i++) {
			const String path = String(paths[i]).strip_edges();
			if (path.is_empty() || !_path_exists(path)) {
				continue;
			}
			Dictionary mention = _path_mention("scene", path);
			if (!_matches_query(mention, p_query)) {
				continue;
			}
			_append_unique(items, seen, mention);
		}
		const String current = String(open.get("current_scene_path", String())).strip_edges();
		if (!current.is_empty() && _path_exists(current)) {
			Dictionary mention = _path_mention("scene", current);
			if (_matches_query(mention, p_query)) {
				_append_unique(items, seen, mention);
			}
		}
	}
	return items;
}

static String _node_mention_path(const Dictionary &p_node) {
	String path = String(p_node.get("path", String())).strip_edges();
	if (path.is_empty()) {
		return String();
	}
	// Prefer path relative to edited scene when absolute under that root.
	EditorInterface *editor = EditorInterface::get_singleton();
	Node *root = editor ? editor->get_edited_scene_root() : nullptr;
	if (root && root->is_inside_tree()) {
		const String root_path = String(root->get_path());
		if (path == root_path) {
			return String(root->get_name());
		}
		if (path.begins_with(root_path + "/")) {
			return path.substr(root_path.length() + 1);
		}
	}
	while (path.begins_with("/")) {
		path = path.substr(1);
	}
	return path;
}

static Array _collect_selection(SolersObservationService *p_observation, const String &p_query) {
	Array items;
	if (!p_observation) {
		return items;
	}
	HashSet<String> seen;
	const Dictionary selection = p_observation->get_selection(0, 0);
	const Array nodes = selection.get("nodes", Array());
	for (int i = 0; i < nodes.size() && items.size() < COLLECT_LIMIT; i++) {
		const Dictionary node = nodes[i];
		if (!(bool)node.get("valid", true)) {
			continue;
		}
		const String path = _node_mention_path(node);
		if (path.is_empty() || !_node_exists(path)) {
			continue;
		}
		const String name = String(node.get("name", path.get_file()));
		Dictionary mention = _path_mention("node", path, name);
		const String node_type = String(node.get("type", String())).strip_edges();
		if (!node_type.is_empty()) {
			mention["type"] = node_type;
		}
		if (!_matches_query(mention, p_query)) {
			continue;
		}
		_append_unique(items, seen, mention);
	}
	return items;
}

String query_at(const String &p_text, int p_caret, int &r_mention_start) {
	r_mention_start = -1;
	if (p_caret < 0 || p_caret > p_text.length()) {
		return String();
	}
	int start = p_caret;
	while (start > 0 && _path_body_char(p_text[start - 1])) {
		start--;
	}
	if (start == 0 || p_text[start - 1] != '@') {
		return String();
	}
	const int at = start - 1;
	if (at > 0 && _id_char(p_text[at - 1])) {
		return String();
	}
	r_mention_start = at;
	return p_text.substr(start, p_caret - start);
}

String format_token(const Dictionary &p_mention) {
	const String source = _source_of(p_mention);
	const String id = String(p_mention.get("id", String())).strip_edges();
	if (id.is_empty()) {
		return String();
	}
	if (source == "plugin") {
		return "@" + id;
	}
	return "@" + source + ":" + id;
}

String prompt_block(const Array &p_mentions) {
	return p_mentions.is_empty() ? String() : "\n\n[Selected Solers context]\n" + JSON::stringify(p_mentions);
}

String dedupe_key(const Dictionary &p_mention) {
	const String id = String(p_mention.get("id", String())).strip_edges();
	if (id.is_empty()) {
		return String();
	}
	return _source_of(p_mention) + "\0" + id;
}

static Dictionary _try_parse_token(const String &p_body) {
	Dictionary empty;
	const String body = p_body.strip_edges();
	if (body.is_empty()) {
		return empty;
	}

	String source;
	String id;
	if (body.begins_with("file:")) {
		source = "file";
		id = body.substr(5);
	} else if (body.begins_with("scene:")) {
		source = "scene";
		id = body.substr(6);
	} else if (body.begins_with("node:")) {
		source = "node";
		id = body.substr(5);
	} else {
		source = "plugin";
		id = body.to_lower();
		for (int i = 0; i < id.length(); i++) {
			if (!_id_char(id[i])) {
				return empty;
			}
		}
	}

	id = id.strip_edges();
	if (id.is_empty()) {
		return empty;
	}

	if (source == "plugin") {
		SolersPlugin *plugin = SolersPluginRegistry::get_plugin(id);
		if (!plugin) {
			return empty;
		}
		return _plugin_mention(plugin);
	}
	if (source == "file" || source == "scene") {
		if (!_path_exists(id)) {
			return empty;
		}
		return _path_mention(source, id);
	}
	if (source == "node") {
		if (!_node_exists(id)) {
			return empty;
		}
		return _path_mention("node", id, id.get_file());
	}
	return empty;
}

Array parse(const String &p_text) {
	Array mentions;
	HashSet<String> seen;
	for (int i = 0; i < p_text.length(); i++) {
		if (p_text[i] != '@' || (i > 0 && _id_char(p_text[i - 1]))) {
			continue;
		}
		int end = i + 1;
		while (end < p_text.length() && _path_body_char(p_text[end]) && p_text[end] != ' ' && p_text[end] != '\t' && p_text[end] != '\n' && p_text[end] != '\r') {
			end++;
		}
		const String body = p_text.substr(i + 1, end - i - 1);
		const Dictionary mention = _try_parse_token(body);
		if (mention.is_empty()) {
			continue;
		}
		const String key = dedupe_key(mention);
		if (key.is_empty() || seen.has(key)) {
			continue;
		}
		seen.insert(key);
		mentions.push_back(mention);
	}
	return mentions;
}

Array scan_line_spans(const String &p_line) {
	Array spans;
	for (int i = 0; i < p_line.length(); i++) {
		if (p_line[i] != '@' || (i > 0 && _id_char(p_line[i - 1]))) {
			continue;
		}
		int end = i + 1;
		while (end < p_line.length() && _path_body_char(p_line[end]) && p_line[end] != ' ' && p_line[end] != '\t' && p_line[end] != '\n' && p_line[end] != '\r') {
			end++;
		}
		const String body = p_line.substr(i + 1, end - i - 1);
		const Dictionary mention = _try_parse_token(body);
		if (mention.is_empty()) {
			continue;
		}
		Dictionary span;
		span["column"] = i;
		span["length"] = end - i;
		span["mention"] = mention;
		spans.push_back(span);
	}
	return spans;
}

Array collect_section_items(const String &p_section_id, SolersObservationService *p_observation, const String &p_query) {
	const String section = p_section_id.strip_edges().to_lower();
	if (section.is_empty()) {
		Array all;
		HashSet<String> seen;
		auto merge = [&](const Array &p_items) {
			for (int i = 0; i < p_items.size(); i++) {
				_append_unique(all, seen, p_items[i]);
			}
		};
		merge(_collect_plugins(p_query));
		merge(_collect_files(p_observation, p_query));
		merge(_collect_scenes(p_observation, p_query));
		merge(_collect_selection(p_observation, p_query));
		return all;
	}
	if (section == "plugins") {
		return _collect_plugins(p_query);
	}
	if (section == "files") {
		return _collect_files(p_observation, p_query);
	}
	if (section == "scenes") {
		return _collect_scenes(p_observation, p_query);
	}
	if (section == "selection") {
		return _collect_selection(p_observation, p_query);
	}
	return Array();
}

Array collect_root_sections(SolersObservationService *p_observation, const String &p_query) {
	struct SectionDef {
		const char *id;
		const char *label;
	};
	static const SectionDef defs[] = {
		{ "plugins", "Plugins" },
		{ "files", "Files" },
		{ "scenes", "Open Scenes" },
		{ "selection", "Selection" },
	};

	Array sections;
	for (const SectionDef &def : defs) {
		const Array items = collect_section_items(def.id, p_observation, p_query);
		if (items.is_empty()) {
			continue;
		}
		Dictionary section;
		section["id"] = def.id;
		section["label"] = String(def.label);
		section["count"] = items.size();
		sections.push_back(section);
	}
	return sections;
}

} // namespace SolersMention
