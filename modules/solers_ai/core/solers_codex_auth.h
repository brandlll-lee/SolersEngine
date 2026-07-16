/**************************************************************************/
/*  solers_codex_auth.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#pragma once

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// Short-lived OAuth state machine for ChatGPT Codex authentication. It owns
// only the browser handoff and token exchange; persistence stays in
// SolersSettingsService and request shaping stays in the LLM protocol.
class SolersCodexAuth {
public:
	enum State {
		STATE_IDLE,
		STATE_WAITING_BROWSER,
		STATE_EXCHANGING,
		STATE_SUCCEEDED,
		STATE_FAILED,
	};

private:
	static constexpr int CALLBACK_PORT = 1455;
	static constexpr uint64_t CALLBACK_TIMEOUT_MSEC = 5 * 60 * 1000;

	Ref<TCPServer> server;
	Ref<StreamPeerTCP> peer;
	String request_buffer;
	String verifier;
	String expected_state;
	String authorization_code;
	String error_message;
	Dictionary completed_tokens;
	uint64_t started_msec = 0;
	State state = STATE_IDLE;

	Thread exchange_thread;
	Mutex result_mutex;
	SafeFlag cancel_requested;
	SafeFlag exchange_done;
	Dictionary worker_result;

	static String _base64_url(const PackedByteArray &p_bytes);
	static PackedByteArray _random_bytes(int p_count);
	static String _query_value(const String &p_target, const String &p_key);
	static String _account_id_from_jwt(const String &p_token);
	static Dictionary _token_request(const Dictionary &p_fields, SafeFlag *p_cancel = nullptr);
	static void _exchange_thread(void *p_userdata);
	void _run_exchange();
	void _respond(const String &p_html, int p_status = 200);
	void _close_listener();

public:
	Dictionary start();
	void poll();
	void cancel();
	Dictionary get_status() const;
	Dictionary take_tokens();
	bool is_active() const { return state == STATE_WAITING_BROWSER || state == STATE_EXCHANGING; }

	// Blocking helper intended for an existing network worker, never the editor
	// main thread. Returns { ok, tokens|error }.
	static Dictionary refresh_tokens(const String &p_refresh_token);

	~SolersCodexAuth();
};
