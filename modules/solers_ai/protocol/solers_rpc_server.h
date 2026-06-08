/**************************************************************************/
/*  solers_rpc_server.h                                                   */
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

#pragma once

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/object/object.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

class SolersMCPAdapter;

class SolersRpcServer : public Object {
	GDCLASS(SolersRpcServer, Object);

	struct Client {
		Ref<StreamPeerTCP> peer;
		String buffer;
		bool authenticated = false;
		uint64_t connected_at_msec = 0;
	};

	Ref<TCPServer> server;
	LocalVector<Client> clients;
	SolersMCPAdapter *mcp_adapter = nullptr;
	String bind_host = "127.0.0.1";
	int requested_port = 6517;
	int local_port = 0;
	String session_token;
	bool running = false;
	int max_clients = 4;
	int max_line_bytes = 1024 * 1024;

	Dictionary _ok(const Variant &p_data) const;
	Dictionary _error(const String &p_code, const String &p_message, bool p_recoverable = true) const;
	void _send_response(const Ref<StreamPeerTCP> &p_peer, const Dictionary &p_response) const;
	void _process_line(Client &r_client, const String &p_line);
	void _drop_client(int p_index);
	String _generate_session_token() const;

protected:
	static void _bind_methods();

public:
	void set_mcp_adapter(SolersMCPAdapter *p_mcp_adapter);
	Dictionary start(const Dictionary &p_args = Dictionary());
	Dictionary stop();
	void poll();
	Dictionary get_status() const;

	SolersRpcServer();
};
