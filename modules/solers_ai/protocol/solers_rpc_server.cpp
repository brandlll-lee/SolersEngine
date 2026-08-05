/**************************************************************************/
/*  solers_rpc_server.cpp                                                 */
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

#include "solers_rpc_server.h"

#include "core/crypto/crypto_core.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "modules/solers_ai/protocol/solers_mcp_adapter.h"

void SolersRpcServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mcp_adapter", "mcp_adapter"), &SolersRpcServer::set_mcp_adapter);
	ClassDB::bind_method(D_METHOD("start", "args"), &SolersRpcServer::start, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("stop"), &SolersRpcServer::stop);
	ClassDB::bind_method(D_METHOD("poll"), &SolersRpcServer::poll);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersRpcServer::get_status);
}

Dictionary SolersRpcServer::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersRpcServer::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

String SolersRpcServer::_generate_session_token() const {
	uint8_t bytes[24];
	CryptoCore::RandomGenerator rng;
	rng.init();
	rng.get_random_bytes(bytes, sizeof(bytes));
	return String::hex_encode_buffer(bytes, sizeof(bytes));
}

void SolersRpcServer::_send_response(const Ref<StreamPeerTCP> &p_peer, const Dictionary &p_response) const {
	if (p_peer.is_null() || p_peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return;
	}
	const String response = JSON::stringify(p_response, "", false, true) + "\n";
	const CharString utf8 = response.utf8();
	p_peer->put_data((const uint8_t *)utf8.get_data(), utf8.length());
}

void SolersRpcServer::_process_line(Client &r_client, const String &p_line) {
	JSON json;
	const Error parse_error = json.parse(p_line);
	if (parse_error != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		Dictionary response;
		response["jsonrpc"] = "2.0";
		response["id"] = Variant();
		Dictionary error;
		error["code"] = -32700;
		error["message"] = "Parse error: expected one JSON-RPC object per line.";
		response["error"] = error;
		_send_response(r_client.peer, response);
		return;
	}

	Dictionary request = json.get_data();
	const Variant id = request.get("id", Variant());
	Dictionary params = request.get("params", Dictionary());

	if (!r_client.authenticated) {
		const String token = params.get("sessionToken", String());
		if (session_token.is_empty() || token != session_token) {
			Dictionary response;
			response["jsonrpc"] = "2.0";
			response["id"] = id;
			Dictionary error;
			error["code"] = -32001;
			error["message"] = "Solers RPC authentication required.";
			Dictionary data;
			data["auth"] = "sessionToken";
			error["data"] = data;
			response["error"] = error;
			_send_response(r_client.peer, response);
			return;
		}
		r_client.authenticated = true;
	}

	if (!mcp_adapter) {
		Dictionary response;
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		Dictionary error;
		error["code"] = -32002;
		error["message"] = "SolersMCPAdapter is unavailable.";
		response["error"] = error;
		_send_response(r_client.peer, response);
		return;
	}

	_send_response(r_client.peer, mcp_adapter->handle_request(request));
}

void SolersRpcServer::_drop_client(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)clients.size());
	if (clients[p_index].peer.is_valid()) {
		clients[p_index].peer->disconnect_from_host();
	}
	clients.remove_at(p_index);
}

void SolersRpcServer::set_mcp_adapter(SolersMCPAdapter *p_mcp_adapter) {
	mcp_adapter = p_mcp_adapter;
}

Dictionary SolersRpcServer::start(const Dictionary &p_args) {
	if (running) {
		return _ok(get_status());
	}
	if (!mcp_adapter) {
		return _error("MCP_ADAPTER_UNAVAILABLE", "SolersMCPAdapter must be configured before starting RPC.", false);
	}

	bind_host = p_args.get("host", "127.0.0.1");
	if (bind_host != "127.0.0.1" && bind_host != "::1") {
		return _error("INVALID_BIND_HOST", "Solers v0.1 RPC only allows loopback bind hosts.");
	}
	requested_port = CLAMP((int)p_args.get("port", 6517), 0, 65535);
	max_clients = CLAMP((int)p_args.get("max_clients", 4), 1, 16);
	max_line_bytes = CLAMP((int)p_args.get("max_line_bytes", 1024 * 1024), 4096, 4 * 1024 * 1024);
	session_token = p_args.get("session_token", String());
	if (session_token.is_empty()) {
		session_token = _generate_session_token();
	}

	server.instantiate();
	const Error err = server->listen(requested_port, IPAddress(bind_host));
	if (err != OK) {
		server.unref();
		return _error("RPC_LISTEN_FAILED", vformat("Unable to listen on %s:%d, error code %d.", bind_host, requested_port, err));
	}

	local_port = server->get_local_port();
	running = true;
	Dictionary data = get_status();
	data["session_token"] = session_token;
	return _ok(data);
}

Dictionary SolersRpcServer::stop() {
	for (int i = clients.size() - 1; i >= 0; i--) {
		_drop_client(i);
	}
	server.unref();
	local_port = 0;
	running = false;
	Dictionary data;
	data["stopped"] = true;
	return _ok(data);
}

void SolersRpcServer::poll() {
	if (!running || server.is_null()) {
		return;
	}

	while ((int)clients.size() < max_clients) {
		Ref<StreamPeerTCP> peer = server->take_connection();
		if (peer.is_null()) {
			break;
		}
		peer->set_no_delay(true);
		Client client;
		client.peer = peer;
		client.connected_at_msec = OS::get_singleton()->get_ticks_msec();
		clients.push_back(client);
	}

	for (int i = clients.size() - 1; i >= 0; i--) {
		Client &client = clients[i];
		if (client.peer.is_null()) {
			_drop_client(i);
			continue;
		}
		client.peer->poll();
		if (client.peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			_drop_client(i);
			continue;
		}

		const int available = client.peer->get_available_bytes();
		if (available <= 0) {
			continue;
		}
		if (client.buffer.length() + available > max_line_bytes) {
			_drop_client(i);
			continue;
		}

		Vector<uint8_t> bytes;
		bytes.resize(available);
		int received = 0;
		const Error err = client.peer->get_partial_data(bytes.ptrw(), available, received);
		if (err != OK || received <= 0) {
			continue;
		}

		client.buffer += String::utf8((const char *)bytes.ptr(), received);
		while (true) {
			const int newline = client.buffer.find("\n");
			if (newline < 0) {
				break;
			}
			String line = client.buffer.substr(0, newline).strip_edges();
			client.buffer = client.buffer.substr(newline + 1);
			if (!line.is_empty()) {
				_process_line(client, line);
			}
			if (client.peer.is_null() || client.peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
				break;
			}
		}
	}
}

Dictionary SolersRpcServer::get_status() const {
	Dictionary status;
	status["running"] = running;
	status["host"] = bind_host;
	status["port"] = local_port;
	status["clients"] = clients.size();
	status["max_clients"] = max_clients;
	status["requires_session_token"] = !session_token.is_empty();
	status["session_token"] = session_token.is_empty() ? String() : "<redacted>";
	return status;
}

SolersRpcServer::SolersRpcServer() {
}
