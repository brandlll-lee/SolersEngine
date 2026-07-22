/**************************************************************************/
/*  solers_plugin_polyhaven.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_plugin_polyhaven.h"

#include "core/io/file_access.h"
#include "core/os/time.h"

Vector<String> SolersPluginPolyHaven::request_headers() {
	Vector<String> headers;
	headers.push_back("User-Agent: Solers Engine development (+https://github.com/brandlll-lee/SolersEngine)");
	headers.push_back("Referer: https://github.com/brandlll-lee/SolersEngine");
	return headers;
}

Dictionary SolersPluginPolyHaven::get_profile() const {
	Dictionary profile;
	profile["id"] = "polyhaven";
	profile["label"] = "Poly Haven";
	Array kinds;
	kinds.push_back("material");
	kinds.push_back("hdri");
	kinds.push_back("3d");
	profile["kinds"] = kinds;
	profile["base_url"] = "https://polyhaven.com";
	profile["api_key_env"] = "";
	profile["docs"] = "https://redocly.github.io/redoc/?url=https://api.polyhaven.com/api-docs/swagger.json";
	profile["description"] = "Free CC0 catalog of textures, HDRIs, and 3D models.";
	profile["attribution"] = "Poly Haven (CC0)";
	profile["license"] = "CC0-1.0";
	profile["requires_api_key"] = false;
	profile["supports_generation"] = false;
	profile["supports_catalog"] = true;
	profile["supports_resume"] = false;
	PackedStringArray download_headers;
	for (const String &header : request_headers()) {
		download_headers.push_back(header);
	}
	profile["download_headers"] = download_headers;
	return profile;
}

Dictionary SolersPluginPolyHaven::_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested) {
	const Vector<String> headers = request_headers();
	Dictionary info_response = http_request("GET", "https://api.polyhaven.com/info/" + p_asset_id.uri_encode(), headers, PackedByteArray(), 60000, 2 * 1024 * 1024, p_cancel_requested);
	if (!(bool)info_response.get("ok", false)) {
		return info_response;
	}
	Dictionary files_response = http_request("GET", "https://api.polyhaven.com/files/" + p_asset_id.uri_encode(), headers, PackedByteArray(), 60000, 8 * 1024 * 1024, p_cancel_requested);
	if (!(bool)files_response.get("ok", false)) {
		return files_response;
	}
	const Dictionary asset = parse_json_body(info_response);
	const Dictionary files = parse_json_body(files_response);
	if (asset.is_empty() || files.is_empty()) {
		Dictionary response;
		response["ok"] = false;
		response["error"] = error_data("POLYHAVEN_BAD_RESPONSE", "Poly Haven did not return complete asset metadata.");
		return response;
	}
	Dictionary response;
	response["ok"] = true;
	response["asset"] = asset;
	response["files"] = files;
	return response;
}

static bool _file_spec_valid(const Dictionary &p_spec) {
	return !String(p_spec.get("url", String())).is_empty() && !String(p_spec.get("md5", String())).is_empty() && (int64_t)p_spec.get("size", -1) >= 0;
}

static int64_t _file_spec_size(const Dictionary &p_spec) {
	int64_t size = (int64_t)p_spec.get("size", 0);
	const Dictionary includes = p_spec.get("include", Dictionary());
	for (const Variant *key = includes.next(nullptr); key; key = includes.next(key)) {
		size += (int64_t)Dictionary(includes[*key]).get("size", 0);
	}
	return size;
}

Array SolersPluginPolyHaven::normalize_variants(const Dictionary &p_files, const String &p_kind) {
	const String kind = p_kind.to_lower();
	Dictionary by_id;
	if (kind == "3d") {
		for (const Variant *format_key = p_files.next(nullptr); format_key; format_key = p_files.next(format_key)) {
			const String format = String(*format_key).to_lower();
			const Dictionary resolutions = p_files[*format_key];
			for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
				const Dictionary formats = resolutions[*resolution_key];
				const Dictionary spec = formats.get(*format_key, Dictionary());
				if (!_file_spec_valid(spec)) {
					continue;
				}
				Dictionary variant;
				variant["id"] = String(*resolution_key).to_lower() + "-" + format;
				variant["resolution"] = String(*resolution_key);
				variant["format"] = String(*format_key);
				variant["size"] = _file_spec_size(spec);
				variant["checksum"] = spec.get("md5", String());
				// The full include list repeats dozens of texture paths per
				// variant; the count is enough signal for choosing one, and
				// acquire resolves the real list from live metadata anyway.
				variant["dependency_count"] = Dictionary(spec.get("include", Dictionary())).size();
				by_id[variant["id"]] = variant;
			}
		}
	} else if (kind == "hdri") {
		const Dictionary resolutions = p_files.get("hdri", Dictionary());
		for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
			const Dictionary formats = resolutions[*resolution_key];
			for (const Variant *format_key = formats.next(nullptr); format_key; format_key = formats.next(format_key)) {
				const Dictionary spec = formats[*format_key];
				if (!_file_spec_valid(spec)) {
					continue;
				}
				Dictionary variant;
				variant["id"] = String(*resolution_key).to_lower() + "-" + String(*format_key).to_lower();
				variant["resolution"] = String(*resolution_key);
				variant["format"] = String(*format_key);
				variant["size"] = _file_spec_size(spec);
				variant["checksum"] = spec.get("md5", String());
				by_id[variant["id"]] = variant;
			}
		}
	} else if (kind == "material") {
		for (const Variant *map_key = p_files.next(nullptr); map_key; map_key = p_files.next(map_key)) {
			if (String(*map_key) == "hdri") {
				continue;
			}
			const Dictionary resolutions = p_files[*map_key];
			for (const Variant *resolution_key = resolutions.next(nullptr); resolution_key; resolution_key = resolutions.next(resolution_key)) {
				const Dictionary formats = resolutions[*resolution_key];
				for (const Variant *format_key = formats.next(nullptr); format_key; format_key = formats.next(format_key)) {
					const Dictionary spec = formats[*format_key];
					if (spec.has("include") || !_file_spec_valid(spec)) {
						continue;
					}
					const String id = String(*resolution_key).to_lower() + "-" + String(*format_key).to_lower();
					Dictionary variant = by_id.get(id, Dictionary());
					variant["id"] = id;
					variant["resolution"] = String(*resolution_key);
					variant["format"] = String(*format_key);
					variant["size"] = (int64_t)variant.get("size", 0) + (int64_t)spec.get("size", 0);
					Array maps = variant.get("maps", Array());
					maps.push_back(String(*map_key));
					maps.sort();
					variant["maps"] = maps;
					Dictionary checksums = variant.get("checksums", Dictionary());
					checksums[String(*map_key)] = spec.get("md5", String());
					variant["checksums"] = checksums;
					by_id[id] = variant;
				}
			}
		}
	}

	Array ids = by_id.keys();
	ids.sort();
	Array variants;
	for (int i = 0; i < ids.size(); i++) {
		variants.push_back(by_id[ids[i]]);
	}
	return variants;
}

bool SolersPluginPolyHaven::_download_file(const Ref<SolersPluginJob> &p_job, const String &p_label, const Dictionary &p_spec, Array &r_files, Dictionary &r_checksums, String &r_error, const String &p_relative_path) {
	const String url = p_spec.get("url", String());
	const String expected_md5 = String(p_spec.get("md5", String())).to_lower();
	const int64_t expected_size = (int64_t)p_spec.get("size", -1);
	if (url.is_empty() || expected_md5.is_empty() || expected_size < 0) {
		r_error = vformat("Poly Haven metadata for %s is incomplete.", p_label);
		return false;
	}
	const Dictionary response = p_job->http_request("GET", url, request_headers(), PackedByteArray(), 180000, expected_size + 1);
	if (!(bool)response.get("ok", false)) {
		r_error = Dictionary(response.get("error", Dictionary())).get("message", vformat("Poly Haven download failed for %s.", p_label));
		return false;
	}
	const PackedByteArray body = response.get("body", PackedByteArray());
	if (body.size() != expected_size) {
		r_error = vformat("Poly Haven download size mismatch for %s.", p_label);
		return false;
	}
	const String source_root = p_job->get_staging_dir().simplify_path();
	String file_path;
	if (!p_relative_path.is_empty()) {
		const String relative_path = p_relative_path.replace_char('\\', '/').simplify_path();
		file_path = source_root.path_join(relative_path).simplify_path();
		if (relative_path.is_empty() || relative_path.is_absolute_path() || !file_path.begins_with(source_root + "/")) {
			r_error = vformat("Poly Haven metadata for %s contains an unsafe package path.", p_label);
			return false;
		}
	} else {
		const String extension = url.get_slice("?", 0).get_extension().to_lower();
		if (extension.is_empty()) {
			r_error = vformat("Poly Haven download for %s has no file extension.", p_label);
			return false;
		}
		file_path = source_root.path_join(safe_slug(p_label) + "." + extension);
	}
	if (!write_bytes_atomic(file_path, body, r_error)) {
		return false;
	}
	const String actual_md5 = FileAccess::get_md5(file_path).to_lower();
	if (actual_md5 != expected_md5) {
		r_error = vformat("Poly Haven checksum mismatch for %s.", p_label);
		return false;
	}
	r_files.push_back(file_path);
	r_checksums[file_path] = actual_md5;
	return true;
}

bool SolersPluginPolyHaven::_download_variant(const Ref<SolersPluginJob> &p_job, const Dictionary &p_files, const String &p_kind, const String &p_variant, Array &r_files, Dictionary &r_maps, Dictionary &r_checksums, String &r_error) {
	const Array available_variants = normalize_variants(p_files, p_kind);
	if (match_catalog_variant_id(available_variants, p_variant).is_empty()) {
		r_error = "The selected Poly Haven variant is not present in the asset's official files metadata.";
		return false;
	}
	const PackedStringArray variant_parts = p_variant.to_lower().split("-", false, 1);
	if (variant_parts.size() != 2) {
		r_error = "Poly Haven variant must use the official resolution-format form, such as 2k-jpg or 2k-hdr.";
		return false;
	}
	const String resolution = variant_parts[0];
	const String format = variant_parts[1];
	if (p_kind == "3d") {
		const Dictionary resolutions = p_files.get(format, Dictionary());
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		const String primary_name = String(spec.get("url", String())).get_slice("?", 0).get_file();
		if (spec.is_empty() || primary_name.is_empty()) {
			r_error = "The selected Poly Haven model variant is not available.";
			return false;
		}
		if (!_download_file(p_job, p_job->get_asset_id() + "-model-" + p_variant, spec, r_files, r_checksums, r_error, primary_name)) {
			return false;
		}
		const String model_path = r_files[r_files.size() - 1];
		const Dictionary includes = spec.get("include", Dictionary());
		Array include_paths = includes.keys();
		include_paths.sort();
		for (int i = 0; i < include_paths.size(); i++) {
			const String relative_path = include_paths[i];
			const Dictionary include_spec = includes[include_paths[i]];
			if (!_download_file(p_job, p_job->get_asset_id() + "-" + relative_path, include_spec, r_files, r_checksums, r_error, relative_path)) {
				return false;
			}
		}
		r_maps["model"] = model_path;
		return true;
	}
	if (p_kind == "hdri") {
		const Dictionary resolutions = p_files.get("hdri", Dictionary());
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		if (spec.is_empty()) {
			r_error = "The selected Poly Haven HDRI variant is not available.";
			return false;
		}
		if (!_download_file(p_job, p_job->get_asset_id() + "-hdri-" + p_variant, spec, r_files, r_checksums, r_error)) {
			return false;
		}
		r_maps["hdri"] = r_files[r_files.size() - 1];
		return true;
	}

	for (const Variant *map_key = p_files.next(nullptr); map_key; map_key = p_files.next(map_key)) {
		const String map_name = String(*map_key);
		if (map_name == "hdri") {
			continue;
		}
		const Dictionary resolutions = p_files[*map_key];
		const Dictionary formats = resolutions.get(resolution, Dictionary());
		const Dictionary spec = formats.get(format, Dictionary());
		if (spec.is_empty() || spec.has("include") || !spec.has("url") || !spec.has("md5") || !spec.has("size")) {
			continue;
		}
		if (!_download_file(p_job, p_job->get_asset_id() + "-" + map_name + "-" + p_variant, spec, r_files, r_checksums, r_error)) {
			return false;
		}
		r_maps[map_name] = r_files[r_files.size() - 1];
	}
	if (r_files.is_empty()) {
		r_error = "The selected Poly Haven material variant has no supported PBR maps.";
		return false;
	}
	return true;
}

Dictionary SolersPluginPolyHaven::catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String query = String(p_args.get("query", String())).strip_edges();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	if (query.is_empty() || (kind != "material" && kind != "hdri" && kind != "3d")) {
		return error_result("INVALID_ARGUMENT", "Poly Haven search requires a query and kind material, hdri, or 3d.");
	}
	const int limit = MAX(1, MIN((int)p_args.get("limit", 20), 50));
	const int offset = MAX(0, (int)p_args.get("offset", 0));
	Dictionary catalog;
	{
		MutexLock lock(catalog_cache_mutex);
		catalog = catalog_cache.get(kind, Dictionary());
	}
	if ((bool)p_args.get("refresh", false) || catalog.is_empty()) {
		const String api_type = kind == "material" ? "textures" : (kind == "hdri" ? "hdris" : "models");
		const Dictionary response = http_request("GET", "https://api.polyhaven.com/assets?type=" + api_type, request_headers(), PackedByteArray(), 60000, 16 * 1024 * 1024, p_cancel_requested);
		if (!(bool)response.get("ok", false)) {
			const Dictionary error = response.get("error", Dictionary());
			if (String(error.get("code", String())) == "HTTP_CANCELLED") {
				return error_result("TOOL_CANCELLED", "Asset catalog search was cancelled.");
			}
			return error_result("CATALOG_SEARCH_FAILED", String(error.get("message", "Poly Haven search failed.")));
		}
		catalog = parse_json_body(response);
		if (catalog.is_empty()) {
			return error_result("CATALOG_BAD_RESPONSE", "Poly Haven did not return catalog metadata.");
		}
		MutexLock lock(catalog_cache_mutex);
		catalog_cache[kind] = catalog.duplicate(true);
	}

	// Poly Haven exposes the authoritative catalog as one metadata object,
	// so query filtering happens locally without additional per-result calls.
	Array catalog_assets;
	for (const Variant *asset_key = catalog.next(nullptr); asset_key; asset_key = catalog.next(asset_key)) {
		const String asset_id = String(*asset_key);
		const Dictionary asset = catalog[*asset_key];
		Dictionary item;
		item["provider"] = "polyhaven";
		item["asset_id"] = asset_id;
		item["kind"] = kind;
		item["display_name"] = asset.get("name", asset_id);
		item["description"] = asset.get("description", String());
		item["tags"] = asset.get("tags", Array());
		item["categories"] = asset.get("categories", Array());
		item["dimensions"] = asset.get("dimensions", Array());
		item["max_resolution"] = asset.get("max_resolution", Array());
		item["polycount"] = asset.get("polycount", 0);
		item["texel_density"] = asset.get("texel_density", 0);
		item["files_hash"] = asset.get("files_hash", String());
		item["source_version"] = asset.get("files_hash", String());
		item["download_count"] = asset.get("download_count", 0);
		item["authors"] = asset.get("authors", Dictionary());
		item["source_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
		item["license"] = "CC0-1.0";
		item["attribution_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
		item["preview_url"] = asset.get("thumbnail_url", String());
		item["details_required"] = true;
		catalog_assets.push_back(item);
	}
	if (p_cancel_requested && p_cancel_requested->is_set()) {
		return error_result("TOOL_CANCELLED", "Asset catalog search was cancelled.");
	}
	const Array ranked = rank_catalog_assets(catalog_assets, query);
	Array assets;
	for (int i = offset; i < MIN(offset + limit, ranked.size()); i++) {
		assets.push_back(ranked[i]);
	}
	Dictionary data;
	data["provider"] = "polyhaven";
	data["kind"] = kind;
	data["assets"] = assets;
	data["count"] = assets.size();
	data["offset"] = offset;
	data["total"] = ranked.size();
	data["next"] = "Call asset.catalog.inspect with one exact provider/kind/asset_id before acquire.";
	return ok_result(data);
}

Dictionary SolersPluginPolyHaven::catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	const String asset_id = String(p_args.get("asset_id", String())).strip_edges();
	if ((kind != "material" && kind != "hdri" && kind != "3d") || asset_id.is_empty() || asset_id.contains("/") || asset_id.contains("\\") || asset_id.contains("..")) {
		return error_result("INVALID_ARGUMENT", "Catalog inspect requires kind and an exact asset_id returned by search.");
	}
	const String inspection_key = kind + "|" + asset_id.to_lower();
	if (!(bool)p_args.get("refresh", false)) {
		MutexLock lock(catalog_cache_mutex);
		const Dictionary cached = catalog_inspections.get(inspection_key, Dictionary());
		if (!cached.is_empty()) {
			return ok_result(cached.duplicate(true));
		}
	}
	const Dictionary metadata_result = _asset_metadata(asset_id, p_cancel_requested);
	if (!(bool)metadata_result.get("ok", false)) {
		const Dictionary error = metadata_result.get("error", Dictionary());
		return error_result(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_INSPECT_FAILED", String(error.get("message", "Poly Haven detail request failed.")));
	}
	const Dictionary asset = metadata_result.get("asset", Dictionary());
	const int asset_type = (int)asset.get("type", -1);
	const String actual_kind = asset_type == 0 ? "hdri" : (asset_type == 1 ? "material" : (asset_type == 2 ? "3d" : String()));
	if (actual_kind != kind) {
		return error_result("CATALOG_KIND_MISMATCH", "The Poly Haven asset does not match the requested kind.");
	}
	const Array variants = normalize_variants(metadata_result.get("files", Dictionary()), kind);
	const String source_version = asset.get("files_hash", String());
	if (variants.is_empty() || source_version.is_empty()) {
		return error_result("CATALOG_DETAILS_INCOMPLETE", "The provider did not return a versioned downloadable variant for this asset.");
	}
	Dictionary item;
	item["display_name"] = asset.get("name", asset_id);
	item["description"] = asset.get("description", String());
	item["tags"] = asset.get("tags", Array());
	item["categories"] = asset.get("categories", Array());
	item["dimensions"] = asset.get("dimensions", Array());
	item["polycount"] = asset.get("polycount", 0);
	item["texel_density"] = asset.get("texel_density", 0);
	item["preview_url"] = asset.get("thumbnail_url", String());
	item["source_url"] = "https://polyhaven.com/a/" + asset_id.uri_encode();
	item["provider"] = "polyhaven";
	item["asset_id"] = asset_id;
	item["kind"] = kind;
	item["source_version"] = source_version;
	item["variants"] = variants;
	item["license"] = "CC0-1.0";
	{
		MutexLock lock(catalog_cache_mutex);
		catalog_inspections[inspection_key] = item.duplicate(true);
	}
	return ok_result(item);
}

void SolersPluginPolyHaven::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	const String kind = state.get("kind", String());
	const String source_asset_id = state.get("source_asset_id", String());
	const String variant = state.get("source_variant", String());
	const Dictionary metadata_result = _asset_metadata(source_asset_id, p_job->cancel_flag());
	if (!(bool)metadata_result.get("ok", false)) {
		state["status"] = "failed";
		state["error"] = metadata_result.get("error", error_data("POLYHAVEN_FAILED", "Poly Haven metadata request failed."));
		p_job->set_state(state);
		return;
	}
	const Dictionary asset = metadata_result.get("asset", Dictionary());
	const int asset_type = (int)asset.get("type", -1);
	String data_type;
	if (asset_type == 0) {
		data_type = "hdri";
	} else if (asset_type == 1) {
		data_type = "material";
	} else if (asset_type == 2) {
		data_type = "3d";
	}
	if (data_type.is_empty()) {
		p_job->fail("POLYHAVEN_UNSUPPORTED_TYPE", "Poly Haven asset is not a supported texture, HDRI, or 3D model.");
		return;
	}
	if (data_type != kind) {
		p_job->fail("CATALOG_KIND_MISMATCH", "The selected Poly Haven asset does not match the requested catalog kind.");
		return;
	}
	state["kind"] = data_type;
	state["asset_type"] = data_type;
	state["dimensions"] = asset.get("dimensions", Array());
	state["polycount"] = asset.get("polycount", 0);
	state["texel_density"] = asset.get("texel_density", 0);
	state["files_hash"] = asset.get("files_hash", String());
	if (String(state.get("source_version", String())) != String(asset.get("files_hash", String()))) {
		p_job->fail("CATALOG_VERSION_CHANGED", "Poly Haven changed this asset after search. Refresh the catalog result and acquire its new files_hash.");
		return;
	}
	state["source_url"] = "https://polyhaven.com/a/" + source_asset_id.uri_encode();
	state["license"] = "CC0-1.0";
	PackedStringArray author_names;
	const Dictionary authors = asset.get("authors", Dictionary());
	for (const Variant *author = authors.next(nullptr); author; author = authors.next(author)) {
		author_names.push_back(String(*author));
	}
	author_names.sort();
	state["attribution"] = vformat("%s by %s on Poly Haven", asset.get("name", source_asset_id), author_names.is_empty() ? String("Poly Haven contributors") : String(", ").join(author_names));
	p_job->set_preview_url(asset.get("thumbnail_url", String()));
	state["stage"] = "downloading";
	p_job->set_state(state);

	Array files;
	Dictionary maps;
	Dictionary checksums;
	String write_error;
	const Dictionary provider_files = metadata_result.get("files", Dictionary());
	if (!_download_variant(p_job, provider_files, data_type, variant, files, maps, checksums, write_error)) {
		p_job->fail("POLYHAVEN_DOWNLOAD_FAILED", write_error);
		return;
	}
	Array map_types = maps.keys();
	map_types.sort();
	state["maps"] = map_types;
	state["map_files"] = maps;
	state["checksums"] = checksums;
	Array entrypoints;
	for (int i = 0; i < map_types.size(); i++) {
		entrypoints.push_back(maps[map_types[i]]);
	}
	state["import_files"] = files;
	state["entrypoints"] = entrypoints;
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	p_job->set_files(files);
	p_job->set_state(state);
}
