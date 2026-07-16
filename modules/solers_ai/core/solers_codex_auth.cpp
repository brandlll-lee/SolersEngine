/**************************************************************************/
/*  solers_codex_auth.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                             */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_codex_auth.h"

#include "core/crypto/crypto.h"
#include "core/crypto/crypto_core.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/time.h"

static const char *SOLERS_CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char *SOLERS_CODEX_ISSUER = "https://auth.openai.com";
static const char *SOLERS_CODEX_REDIRECT_URI = "http://localhost:1455/auth/callback";

static Dictionary _codex_error(const String &p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary out;
	out["ok"] = false;
	out["error"] = error;
	return out;
}

PackedByteArray SolersCodexAuth::_random_bytes(int p_count) {
	Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
	return crypto.is_valid() ? crypto->generate_random_bytes(p_count) : PackedByteArray();
}

String SolersCodexAuth::_base64_url(const PackedByteArray &p_bytes) {
	if (p_bytes.is_empty()) {
		return String();
	}
	return CryptoCore::b64_encode_str(p_bytes.ptr(), p_bytes.size()).replace("+", "-").replace("/", "_").trim_suffix("=").trim_suffix("=");
}

String SolersCodexAuth::_query_value(const String &p_target, const String &p_key) {
	const int query_pos = p_target.find("?");
	if (query_pos < 0) {
		return String();
	}
	const PackedStringArray pairs = p_target.substr(query_pos + 1).split("&");
	for (const String &pair : pairs) {
		const int equals = pair.find("=");
		const String key = equals < 0 ? pair : pair.substr(0, equals);
		if (key.uri_decode() == p_key) {
			return (equals < 0 ? String() : pair.substr(equals + 1)).uri_decode();
		}
	}
	return String();
}

String SolersCodexAuth::_account_id_from_jwt(const String &p_token) {
	const PackedStringArray parts = p_token.split(".");
	if (parts.size() != 3) {
		return String();
	}
	String payload = parts[1].replace("-", "+").replace("_", "/");
	while ((payload.length() % 4) != 0) {
		payload += "=";
	}
	const CharString encoded = payload.utf8();
	Vector<uint8_t> decoded;
	decoded.resize(encoded.length());
	size_t decoded_len = 0;
	if (CryptoCore::b64_decode(decoded.ptrw(), decoded.size(), &decoded_len, (const uint8_t *)encoded.ptr(), encoded.length()) != OK) {
		return String();
	}
	const Variant parsed = JSON::parse_string(String::utf8((const char *)decoded.ptr(), decoded_len));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary claims = parsed;
	String account_id = claims.get("chatgpt_account_id", String());
	if (!account_id.is_empty()) {
		return account_id;
	}
	const Dictionary auth = claims.get("https://api.openai.com/auth", Dictionary());
	account_id = auth.get("chatgpt_account_id", String());
	if (!account_id.is_empty()) {
		return account_id;
	}
	const Array organizations = claims.get("organizations", Array());
	return organizations.is_empty() ? String() : String(Dictionary(organizations[0]).get("id", String()));
}

Dictionary SolersCodexAuth::_token_request(const Dictionary &p_fields, SafeFlag *p_cancel) {
	String form;
	for (const Variant *key = p_fields.next(nullptr); key; key = p_fields.next(key)) {
		if (!form.is_empty()) {
			form += "&";
		}
		form += String(*key).uri_encode() + "=" + String(p_fields[*key]).uri_encode();
	}

	Ref<HTTPClient> http = HTTPClient::create();
	if (http.is_null()) {
		return _codex_error("HTTP_UNAVAILABLE", "Could not create the OAuth HTTP client.");
	}
	if (http->connect_to_host("auth.openai.com", 443, TLSOptions::client()) != OK) {
		return _codex_error("OAUTH_CONNECT_FAILED", "Could not connect to OpenAI authentication.");
	}

	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 60000;
	bool requested = false;
	bool reading_body = false;
	int response_code = 0;
	PackedByteArray response_body;
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		if (p_cancel && p_cancel->is_set()) {
			return _codex_error("OAUTH_CANCELLED", "ChatGPT authorization was cancelled.");
		}
		http->poll();
		const HTTPClient::Status status = http->get_status();
		if (status == HTTPClient::STATUS_RESOLVING || status == HTTPClient::STATUS_CONNECTING || status == HTTPClient::STATUS_REQUESTING) {
			OS::get_singleton()->delay_usec(10000);
			continue;
		}
		if (status == HTTPClient::STATUS_CONNECTED) {
			if (!requested) {
				Vector<String> headers;
				headers.push_back("Content-Type: application/x-www-form-urlencoded");
				headers.push_back("Accept: application/json");
				headers.push_back("User-Agent: Solers");
				const CharString bytes = form.utf8();
				if (http->request(HTTPClient::METHOD_POST, "/oauth/token", headers, (const uint8_t *)bytes.ptr(), bytes.length()) != OK) {
					return _codex_error("OAUTH_REQUEST_FAILED", "Could not send the OAuth token request.");
				}
				requested = true;
				continue;
			}
			if (reading_body) {
				break;
			}
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		if (status == HTTPClient::STATUS_BODY) {
			if (!reading_body) {
				reading_body = true;
				response_code = http->get_response_code();
			}
			const PackedByteArray chunk = http->read_response_body_chunk();
			if (!chunk.is_empty()) {
				response_body.append_array(chunk);
			}
			OS::get_singleton()->delay_usec(5000);
			continue;
		}
		if (status == HTTPClient::STATUS_DISCONNECTED) {
			break;
		}
		return _codex_error("OAUTH_TRANSPORT_FAILED", "OpenAI authentication could not be reached.");
	}

	if (!reading_body) {
		return _codex_error("OAUTH_TIMEOUT", "OpenAI authentication timed out.");
	}
	const String body = response_body.is_empty() ? String() : String::utf8((const char *)response_body.ptr(), response_body.size());
	const Variant parsed = JSON::parse_string(body);
	if (response_code < 200 || response_code >= 300) {
		String message = vformat("OpenAI authentication returned HTTP %d.", response_code);
		if (parsed.get_type() == Variant::DICTIONARY) {
			const Dictionary payload = parsed;
			const Variant error_value = payload.get("error", Variant());
			if (error_value.get_type() == Variant::DICTIONARY) {
				message = Dictionary(error_value).get("message", message);
			} else if (error_value.get_type() == Variant::STRING) {
				message = error_value;
			}
		}
		return _codex_error("OAUTH_TOKEN_REJECTED", message);
	}
	if (parsed.get_type() != Variant::DICTIONARY) {
		return _codex_error("OAUTH_RESPONSE_INVALID", "OpenAI returned an invalid token response.");
	}

	const Dictionary payload = parsed;
	const String access = payload.get("access_token", String());
	const String refresh = payload.get("refresh_token", String());
	if (access.is_empty()) {
		return _codex_error("OAUTH_RESPONSE_INVALID", "OpenAI returned no access token.");
	}
	Dictionary tokens;
	tokens["type"] = "oauth";
	tokens["access"] = access;
	tokens["refresh"] = refresh;
	tokens["expires_at"] = (int64_t)Time::get_singleton()->get_unix_time_from_system() + (int64_t)payload.get("expires_in", 3600);
	String account_id = _account_id_from_jwt(payload.get("id_token", String()));
	if (account_id.is_empty()) {
		account_id = _account_id_from_jwt(access);
	}
	if (!account_id.is_empty()) {
		tokens["account_id"] = account_id;
	}
	Dictionary out;
	out["ok"] = true;
	out["tokens"] = tokens;
	return out;
}

void SolersCodexAuth::_exchange_thread(void *p_userdata) {
	static_cast<SolersCodexAuth *>(p_userdata)->_run_exchange();
}

void SolersCodexAuth::_run_exchange() {
	Dictionary fields;
	fields["grant_type"] = "authorization_code";
	fields["code"] = authorization_code;
	fields["redirect_uri"] = SOLERS_CODEX_REDIRECT_URI;
	fields["client_id"] = SOLERS_CODEX_CLIENT_ID;
	fields["code_verifier"] = verifier;
	const Dictionary result = _token_request(fields, &cancel_requested);
	{
		MutexLock lock(result_mutex);
		worker_result = result;
	}
	exchange_done.set();
}

void SolersCodexAuth::_respond(const String &p_html, int p_status) {
	if (peer.is_null()) {
		return;
	}
	const CharString html = p_html.utf8();
	const String status_text = p_status == 200 ? "OK" : "Bad Request";
	const String response = vformat("HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", p_status, status_text, html.length()) + p_html;
	const CharString bytes = response.utf8();
	peer->put_data((const uint8_t *)bytes.ptr(), bytes.length());
	peer->disconnect_from_host();
	peer.unref();
}

void SolersCodexAuth::_close_listener() {
	if (peer.is_valid()) {
		peer->disconnect_from_host();
		peer.unref();
	}
	server.unref();
	request_buffer = String();
}

Dictionary SolersCodexAuth::start() {
	if (is_active()) {
		return _codex_error("OAUTH_BUSY", "ChatGPT authorization is already in progress.");
	}
	if (exchange_thread.is_started()) {
		exchange_thread.wait_to_finish();
	}
	cancel_requested.clear();
	exchange_done.clear();
	worker_result.clear();
	completed_tokens.clear();
	error_message = String();
	request_buffer = String();

	const PackedByteArray verifier_bytes = _random_bytes(32);
	const PackedByteArray state_bytes = _random_bytes(32);
	if (verifier_bytes.is_empty() || state_bytes.is_empty()) {
		state = STATE_FAILED;
		error_message = "Secure random generation is unavailable.";
		return _codex_error("OAUTH_RANDOM_FAILED", error_message);
	}
	verifier = _base64_url(verifier_bytes);
	expected_state = _base64_url(state_bytes);
	const CharString verifier_utf8 = verifier.utf8();
	uint8_t digest[32];
	CryptoCore::SHA256Context sha;
	sha.start();
	sha.update((const uint8_t *)verifier_utf8.ptr(), verifier_utf8.length());
	sha.finish(digest);
	PackedByteArray digest_bytes;
	digest_bytes.resize(32);
	memcpy(digest_bytes.ptrw(), digest, 32);
	const String challenge = _base64_url(digest_bytes);

	server.instantiate();
	const Error listen_error = server->listen(CALLBACK_PORT, IPAddress("127.0.0.1"));
	if (listen_error != OK) {
		server.unref();
		state = STATE_FAILED;
		error_message = "Port 1455 is already in use. Close Codex CLI or another login window and try again.";
		return _codex_error("OAUTH_PORT_BUSY", error_message);
	}

	const String scope = "openid profile email offline_access";
	const String url = String(SOLERS_CODEX_ISSUER) + "/oauth/authorize?" +
			"response_type=code&client_id=" + String(SOLERS_CODEX_CLIENT_ID).uri_encode() +
			"&redirect_uri=" + String(SOLERS_CODEX_REDIRECT_URI).uri_encode() +
			"&scope=" + scope.uri_encode() +
			"&code_challenge=" + challenge.uri_encode() +
			"&code_challenge_method=S256&id_token_add_organizations=true" +
			"&codex_cli_simplified_flow=true&state=" + expected_state.uri_encode() +
			"&originator=solers";
	started_msec = OS::get_singleton()->get_ticks_msec();
	state = STATE_WAITING_BROWSER;
	const Error browser_error = OS::get_singleton()->shell_open(url);
	if (browser_error != OK) {
		_close_listener();
		state = STATE_FAILED;
		error_message = "Could not open the system browser for ChatGPT authorization.";
		return _codex_error("OAUTH_BROWSER_FAILED", error_message);
	}
	Dictionary out;
	out["ok"] = true;
	out["status"] = get_status();
	return out;
}

void SolersCodexAuth::poll() {
	if (state == STATE_WAITING_BROWSER) {
		if (OS::get_singleton()->get_ticks_msec() - started_msec > CALLBACK_TIMEOUT_MSEC) {
			error_message = "ChatGPT authorization timed out.";
			state = STATE_FAILED;
			_close_listener();
			return;
		}
		if (peer.is_null() && server.is_valid()) {
			peer = server->take_connection();
			if (peer.is_valid()) {
				peer->set_no_delay(true);
			}
		}
		if (peer.is_valid()) {
			peer->poll();
			const int available = peer->get_available_bytes();
			if (available > 0 && request_buffer.length() + available < 32768) {
				Vector<uint8_t> bytes;
				bytes.resize(available);
				int received = 0;
				if (peer->get_partial_data(bytes.ptrw(), available, received) == OK && received > 0) {
					request_buffer += String::utf8((const char *)bytes.ptr(), received);
				}
			}
			if (request_buffer.contains("\r\n\r\n")) {
				const String request_line = request_buffer.get_slice("\r\n", 0);
				const String target = request_line.get_slice(" ", 1);
				const String path = target.get_slice("?", 0);
				const String error = _query_value(target, "error");
				const String code = _query_value(target, "code");
				const String callback_state = _query_value(target, "state");
				if (path != "/auth/callback") {
					_respond("<!doctype html><title>Solers</title><h1>Not found</h1>", 400);
					request_buffer = String();
					return;
				}
				if (!error.is_empty()) {
					error_message = "ChatGPT authorization was declined.";
					_respond("<!doctype html><title>Solers authorization</title><h1>Authorization cancelled</h1><p>You can close this window and return to Solers.</p>");
					state = STATE_FAILED;
					_close_listener();
					return;
				}
				if (code.is_empty() || callback_state != expected_state) {
					error_message = code.is_empty() ? "The authorization callback contained no code." : "The authorization state did not match.";
					_respond("<!doctype html><title>Solers authorization</title><h1>Authorization failed</h1><p>Return to Solers and try again.</p>", 400);
					state = STATE_FAILED;
					_close_listener();
					return;
				}
				authorization_code = code;
				_respond("<!doctype html><title>Solers authorization</title><style>body{font-family:system-ui;background:#151515;color:#eee;display:grid;place-items:center;height:100vh;margin:0}main{text-align:center}p{color:#aaa}</style><main><h1>Authorization received</h1><p>You can close this window and return to Solers.</p></main>");
				_close_listener();
				state = STATE_EXCHANGING;
				if (exchange_thread.start(&SolersCodexAuth::_exchange_thread, this) != OK) {
					state = STATE_FAILED;
					error_message = "Could not start the OAuth token exchange.";
				}
			}
		}
	}

	if (state == STATE_EXCHANGING && exchange_done.is_set()) {
		exchange_thread.wait_to_finish();
		Dictionary result;
		{
			MutexLock lock(result_mutex);
			result = worker_result;
		}
		if (result.get("ok", false)) {
			completed_tokens = result.get("tokens", Dictionary());
			state = STATE_SUCCEEDED;
		} else {
			error_message = Dictionary(result.get("error", Dictionary())).get("message", "ChatGPT authorization failed.");
			state = STATE_FAILED;
		}
	}
}

void SolersCodexAuth::cancel() {
	cancel_requested.set();
	_close_listener();
	if (state == STATE_WAITING_BROWSER || state == STATE_EXCHANGING) {
		state = STATE_FAILED;
		error_message = "ChatGPT authorization was cancelled.";
	}
}

Dictionary SolersCodexAuth::get_status() const {
	Dictionary out;
	String state_name = "idle";
	if (state == STATE_WAITING_BROWSER) {
		state_name = "waiting_browser";
	} else if (state == STATE_EXCHANGING) {
		state_name = "exchanging";
	} else if (state == STATE_SUCCEEDED) {
		state_name = "succeeded";
	} else if (state == STATE_FAILED) {
		state_name = "failed";
	}
	out["state"] = state_name;
	out["active"] = is_active();
	if (!error_message.is_empty()) {
		out["error"] = error_message;
	}
	return out;
}

Dictionary SolersCodexAuth::take_tokens() {
	if (state != STATE_SUCCEEDED) {
		return Dictionary();
	}
	Dictionary out = completed_tokens;
	completed_tokens.clear();
	state = STATE_IDLE;
	return out;
}

Dictionary SolersCodexAuth::refresh_tokens(const String &p_refresh_token) {
	if (p_refresh_token.is_empty()) {
		return _codex_error("OAUTH_REFRESH_MISSING", "The ChatGPT refresh token is missing.");
	}
	Dictionary fields;
	fields["grant_type"] = "refresh_token";
	fields["refresh_token"] = p_refresh_token;
	fields["client_id"] = SOLERS_CODEX_CLIENT_ID;
	Dictionary result = _token_request(fields);
	if (result.get("ok", false)) {
		Dictionary tokens = result.get("tokens", Dictionary());
		if (String(tokens.get("refresh", String())).is_empty()) {
			tokens["refresh"] = p_refresh_token;
		}
		result["tokens"] = tokens;
	}
	return result;
}

SolersCodexAuth::~SolersCodexAuth() {
	cancel_requested.set();
	_close_listener();
	if (exchange_thread.is_started()) {
		exchange_thread.wait_to_finish();
	}
}
