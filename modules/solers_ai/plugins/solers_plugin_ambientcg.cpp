/**************************************************************************/
/*  solers_plugin_ambientcg.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "solers_plugin_ambientcg.h"

#include "core/io/json.h"
#include "core/os/time.h"

#include "modules/zip/zip_reader.h"

Dictionary SolersPluginAmbientCG::get_profile() const {
	Dictionary profile;
	profile["id"] = "ambientcg";
	profile["label"] = "ambientCG";
	Array kinds;
	kinds.push_back("material");
	kinds.push_back("hdri");
	kinds.push_back("3d");
	profile["kinds"] = kinds;
	profile["base_url"] = "https://ambientcg.com";
	profile["api_key_env"] = "";
	profile["docs"] = "https://docs.ambientcg.com";
	profile["description"] = "Free CC0 catalog of PBR materials, HDRIs, and 3D models.";
	profile["attribution"] = "ambientCG (CC0)";
	profile["license"] = "CC0-1.0";
	profile["requires_api_key"] = false;
	profile["supports_generation"] = false;
	profile["supports_catalog"] = true;
	profile["supports_resume"] = false;
	return profile;
}

Dictionary SolersPluginAmbientCG::_asset_metadata(const String &p_asset_id, const SafeFlag *p_cancel_requested) {
	const String url = "https://ambientcg.com/api/v3/assets?id=" + p_asset_id.uri_encode() + "&limit=1&include=id,type,title,url,tags,dimensions,maps,downloads,thumbnails";
	Dictionary response = http_request(HTTPClient::METHOD_GET, url, Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
	if (!(bool)response.get("ok", false)) {
		return response;
	}
	const Dictionary root = parse_json_body(response);
	const Array assets = root.get("assets", Array());
	for (int i = 0; i < assets.size(); i++) {
		const Dictionary asset = assets[i];
		if (String(asset.get("id", String())).nocasecmp_to(p_asset_id) == 0) {
			response["asset"] = asset;
			return response;
		}
	}
	response["ok"] = false;
	response["error"] = error_data("AMBIENTCG_NOT_FOUND", "ambientCG did not return the requested asset id.");
	return response;
}

static Dictionary _download_package(const Dictionary &p_asset, const String &p_package) {
	const Array downloads = p_asset.get("downloads", Array());
	for (int i = 0; i < downloads.size(); i++) {
		const Dictionary download = downloads[i];
		if (String(download.get("attributes", String())).nocasecmp_to(p_package) == 0 && String(download.get("extension", String())).to_lower() == "zip") {
			return download;
		}
	}
	return Dictionary();
}

bool SolersPluginAmbientCG::_extract_archive(const Ref<SolersPluginJob> &p_job, const PackedByteArray &p_archive, Array &r_files, String &r_error) {
	const String archive_path = p_job->get_staging_dir().path_join("ambientcg.zip");
	if (!write_bytes_atomic(archive_path, p_archive, r_error)) {
		return false;
	}
	Ref<ZIPReader> reader;
	reader.instantiate();
	if (reader->open(archive_path) != OK) {
		r_error = "ambientCG download is not a readable ZIP archive.";
		return false;
	}
	const String source_root = p_job->get_staging_dir().simplify_path();
	const PackedStringArray entries = reader->get_files();
	for (int i = 0; i < entries.size(); i++) {
		if (p_job->is_cancelled()) {
			r_error = "ambientCG extraction was cancelled.";
			reader->close();
			return false;
		}
		const String entry = entries[i];
		if (entry.is_empty() || entry.ends_with("/") || entry.begins_with("/") || entry.contains("\\")) {
			continue;
		}
		const PackedStringArray parts = entry.split("/", false);
		bool unsafe = false;
		for (int part = 0; part < parts.size(); part++) {
			if (parts[part] == "." || parts[part] == "..") {
				unsafe = true;
				break;
			}
		}
		const String output_path = source_root.path_join(entry).simplify_path();
		if (unsafe || !output_path.begins_with(source_root + "/")) {
			r_error = "ambientCG archive contains an unsafe path.";
			reader->close();
			return false;
		}
		const PackedByteArray bytes = reader->read_file(entry, true);
		if (bytes.is_empty()) {
			continue;
		}
		if (!write_bytes_atomic(output_path, bytes, r_error)) {
			reader->close();
			return false;
		}
		r_files.push_back(output_path);
	}
	reader->close();
	if (r_files.is_empty()) {
		r_error = "ambientCG archive did not contain importable files.";
		return false;
	}
	return true;
}

Dictionary SolersPluginAmbientCG::catalog_search(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
	const String query = String(p_args.get("query", String())).strip_edges();
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	if (kind != "material" && kind != "hdri" && kind != "3d") {
		return error_result("INVALID_ARGUMENT", "ambientCG catalog requires kind material, hdri, or 3d.");
	}
	const int limit = MAX(1, MIN((int)p_args.get("limit", 20), 50));
	const int offset = MAX(0, (int)p_args.get("offset", 0));
	const String api_type = kind == "3d" ? "3d-model" : kind;
	const String url = "https://ambientcg.com/api/v3/assets?q=" + query.uri_encode() + "&type=" + api_type + "&sort=popular&limit=" + itos(limit) + "&offset=" + itos(offset) + "&include=id,type,title,url,tags,dimensions,thumbnails";
	const Dictionary response = http_request(HTTPClient::METHOD_GET, url, Vector<String>(), PackedByteArray(), 60000, 4 * 1024 * 1024, p_cancel_requested);
	if (!(bool)response.get("ok", false)) {
		const Dictionary error = response.get("error", Dictionary());
		return error_result(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_SEARCH_FAILED", String(error.get("message", "ambientCG catalog request failed.")));
	}
	const Dictionary root = parse_json_body(response);
	const Array catalog_assets = root.get("assets", Array());
	Array assets;
	for (int i = 0; i < catalog_assets.size(); i++) {
		const Dictionary asset = catalog_assets[i];
		Dictionary item;
		item["provider"] = "ambientcg";
		item["asset_id"] = asset.get("id", String());
		item["kind"] = kind;
		item["display_name"] = asset.get("title", String());
		item["tags"] = asset.get("tags", Array());
		item["dimensions"] = asset.get("dimensions", Dictionary());
		item["source_url"] = asset.get("url", String());
		const Dictionary thumbnails = asset.get("thumbnails", Dictionary());
		item["preview_url"] = thumbnails.get("512-JPG-242424", thumbnails.get("512-PNG", String()));
		item["license"] = "CC0-1.0";
		item["details_required"] = true;
		assets.push_back(item);
	}
	Dictionary data;
	data["provider"] = "ambientcg";
	data["kind"] = kind;
	data["assets"] = assets;
	data["count"] = assets.size();
	data["offset"] = offset;
	data["total"] = root.get("totalResults", assets.size());
	data["next"] = "Call asset.catalog.inspect with one exact provider/kind/asset_id before acquire.";
	return ok_result(data);
}

Dictionary SolersPluginAmbientCG::catalog_inspect(const Dictionary &p_args, const SafeFlag *p_cancel_requested) {
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
		return error_result(String(error.get("code", String())) == "HTTP_CANCELLED" ? "TOOL_CANCELLED" : "CATALOG_INSPECT_FAILED", String(error.get("message", "ambientCG detail request failed.")));
	}
	const Dictionary asset = metadata_result.get("asset", Dictionary());
	const String asset_type = String(asset.get("type", String())).to_lower();
	const String actual_kind = asset_type == "3d-model" ? "3d" : asset_type;
	if (actual_kind != kind) {
		return error_result("CATALOG_KIND_MISMATCH", "The ambientCG asset does not match the requested kind.");
	}
	Array variants;
	const Array downloads = asset.get("downloads", Array());
	for (int i = 0; i < downloads.size(); i++) {
		const Dictionary download = downloads[i];
		if (String(download.get("extension", String())).to_lower() != "zip") {
			continue;
		}
		Dictionary variant;
		variant["id"] = download.get("attributes", String());
		variant["size"] = download.get("size", 0);
		variant["checksum_available"] = false;
		variants.push_back(variant);
	}
	const String source_version = JSON::stringify(downloads).sha256_text();
	if (variants.is_empty() || source_version.is_empty()) {
		return error_result("CATALOG_DETAILS_INCOMPLETE", "The provider did not return a versioned downloadable variant for this asset.");
	}
	Dictionary item;
	item["display_name"] = asset.get("title", asset_id);
	item["tags"] = asset.get("tags", Array());
	item["maps"] = asset.get("maps", Array());
	item["dimensions"] = asset.get("dimensions", Dictionary());
	item["source_url"] = asset.get("url", String());
	const Dictionary thumbnails = asset.get("thumbnails", Dictionary());
	item["preview_url"] = thumbnails.get("512-JPG-242424", thumbnails.get("512-PNG", String()));
	item["provider"] = "ambientcg";
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

void SolersPluginAmbientCG::run_job(const Ref<SolersPluginJob> &p_job) {
	Dictionary state = p_job->get_state();
	const String kind = state.get("kind", String());
	const String source_asset_id = state.get("source_asset_id", String());
	const String variant = state.get("source_variant", String());
	const Dictionary metadata_result = _asset_metadata(source_asset_id, p_job->cancel_flag());
	if (!(bool)metadata_result.get("ok", false)) {
		state["status"] = "failed";
		state["error"] = metadata_result.get("error", error_data("AMBIENTCG_FAILED", "ambientCG metadata request failed."));
		p_job->set_state(state);
		return;
	}
	const Dictionary asset = metadata_result.get("asset", Dictionary());
	const Dictionary download_spec = _download_package(asset, variant);
	if (download_spec.is_empty()) {
		p_job->fail("AMBIENTCG_PACKAGE_NOT_FOUND", "The selected ambientCG variant is not available for this asset.");
		return;
	}
	const String url = download_spec.get("url", String());
	const int64_t size = (int64_t)download_spec.get("size", -1);
	if (url.is_empty() || size < 0) {
		p_job->fail("AMBIENTCG_BAD_METADATA", "ambientCG package metadata is incomplete.");
		return;
	}
	state["download_url"] = url;
	state["download_bytes"] = size;
	state["source_url"] = asset.get("url", String());
	state["license"] = "CC0-1.0";
	state["asset_type"] = asset.get("type", String());
	state["maps"] = asset.get("maps", Array());
	state["dimensions"] = asset.get("dimensions", Dictionary());
	const Dictionary thumbnails = asset.get("thumbnails", Dictionary());
	p_job->set_preview_url(thumbnails.get("512-JPG-242424", thumbnails.get("512-PNG", String())));
	state["stage"] = "downloading";
	p_job->set_state(state);

	Dictionary archive = p_job->http_request(HTTPClient::METHOD_GET, url, Vector<String>(), PackedByteArray(), 180000, size + 1);
	if (!(bool)archive.get("ok", false)) {
		state["status"] = "failed";
		state["error"] = archive.get("error", Dictionary());
		p_job->set_state(state);
		return;
	}
	const String provider_type = String(asset.get("type", String())).to_lower();
	const String data_type = provider_type == "3d-model" ? "3d" : provider_type;
	if (data_type != "material" && data_type != "hdri" && data_type != "3d") {
		p_job->fail("AMBIENTCG_UNSUPPORTED_TYPE", "ambientCG asset is not a supported material, HDRI, or 3D model.");
		return;
	}
	if (data_type != kind) {
		p_job->fail("CATALOG_KIND_MISMATCH", "The selected ambientCG asset does not match the requested catalog kind.");
		return;
	}
	const String current_version = JSON::stringify(asset.get("downloads", Array())).sha256_text();
	if (String(state.get("source_version", String())) != current_version) {
		p_job->fail("CATALOG_VERSION_CHANGED", "ambientCG changed this asset after inspection. Inspect it again before acquiring.");
		return;
	}
	Array files;
	String write_error;
	if (!_extract_archive(p_job, archive.get("body", PackedByteArray()), files, write_error)) {
		p_job->fail("AMBIENTCG_EXTRACT_FAILED", write_error);
		return;
	}
	state["kind"] = data_type;
	Array entrypoints;
	Dictionary import_selection;
	if (data_type == "3d") {
		import_selection = project_import_selection(files, Array(), Array());
		if ((bool)import_selection.get("valid", false)) {
			entrypoints = model_resource_entrypoints(import_selection.get("files", Array()));
		}
	} else {
		entrypoints = native_resource_entrypoints(files);
		import_selection = project_import_selection(files, Array(), entrypoints);
	}
	if (entrypoints.is_empty() || !(bool)import_selection.get("valid", false)) {
		p_job->fail("AMBIENTCG_PACKAGE_HAS_NO_NATIVE_RESOURCE", "ambientCG package has no loadable native resource for the requested asset kind.");
		return;
	}
	state["import_files"] = import_selection.get("files", Array());
	state["entrypoints"] = entrypoints;
	state["updated_at"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	p_job->set_files(files);
	p_job->set_state(state);
}
