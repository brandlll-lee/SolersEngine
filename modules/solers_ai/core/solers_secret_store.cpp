/**************************************************************************/
/*  solers_secret_store.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/

#include "solers_secret_store.h"

#include "core/crypto/crypto.h"
#include "core/crypto/crypto_core.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"

#ifdef WINDOWS_ENABLED
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dpapi.h> // CryptProtectData / CryptUnprotectData (links against crypt32, already in platform libs).
#endif

static const char *SOLERS_SECRET_DPAPI_PREFIX = "sdpapi$";
static const char *SOLERS_SECRET_AES_PREFIX = "saes1$";
// Entropy salt mixed into both backends so other apps' DPAPI blobs / AES keys
// never collide with ours.
static const char *SOLERS_SECRET_SALT = "solers-byok-secret-v1";

bool SolersSecretStore::is_protected(const String &p_stored) {
	return p_stored.begins_with(SOLERS_SECRET_DPAPI_PREFIX) || p_stored.begins_with(SOLERS_SECRET_AES_PREFIX);
}

#ifdef WINDOWS_ENABLED
static String _dpapi_protect(const String &p_plain) {
	const CharString utf8 = p_plain.utf8();
	const CharString salt = String(SOLERS_SECRET_SALT).utf8();

	DATA_BLOB in_blob;
	in_blob.pbData = (BYTE *)utf8.ptr();
	in_blob.cbData = (DWORD)utf8.length();

	DATA_BLOB entropy;
	entropy.pbData = (BYTE *)salt.ptr();
	entropy.cbData = (DWORD)salt.length();

	DATA_BLOB out_blob = {};
	if (!CryptProtectData(&in_blob, L"Solers BYOK API key", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
		return String();
	}

	Vector<uint8_t> bytes;
	bytes.resize(out_blob.cbData);
	memcpy(bytes.ptrw(), out_blob.pbData, out_blob.cbData);
	LocalFree(out_blob.pbData);

	return String(SOLERS_SECRET_DPAPI_PREFIX) + CryptoCore::b64_encode_str(bytes.ptr(), bytes.size());
}

static String _dpapi_unprotect(const String &p_payload) {
	const CharString b64 = p_payload.utf8();
	Vector<uint8_t> bytes;
	bytes.resize(b64.length()); // Decoded data is always shorter than base64 text.
	size_t decoded_len = 0;
	if (CryptoCore::b64_decode(bytes.ptrw(), bytes.size(), &decoded_len, (const uint8_t *)b64.ptr(), b64.length()) != OK) {
		return String();
	}

	const CharString salt = String(SOLERS_SECRET_SALT).utf8();

	DATA_BLOB in_blob;
	in_blob.pbData = bytes.ptrw();
	in_blob.cbData = (DWORD)decoded_len;

	DATA_BLOB entropy;
	entropy.pbData = (BYTE *)salt.ptr();
	entropy.cbData = (DWORD)salt.length();

	DATA_BLOB out_blob = {};
	if (!CryptUnprotectData(&in_blob, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
		return String();
	}

	String plain = String::utf8((const char *)out_blob.pbData, out_blob.cbData);
	SecureZeroMemory(out_blob.pbData, out_blob.cbData);
	LocalFree(out_blob.pbData);
	return plain;
}
#endif // WINDOWS_ENABLED

// AES-256-CBC keyed from SHA-256(machine unique id + salt). Weaker than a real
// OS keystore (the key is derivable on the same machine) but it keeps secrets
// out of plaintext settings files and ties them to this device.
static void _aes_machine_key(uint8_t r_key[32]) {
	const CharString seed = (OS::get_singleton()->get_unique_id() + String(":") + String(SOLERS_SECRET_SALT)).utf8();
	CryptoCore::SHA256Context sha;
	sha.start();
	sha.update((const uint8_t *)seed.ptr(), seed.length());
	sha.finish(r_key);
}

String SolersSecretStore::_aes_protect(const String &p_plain) {
	uint8_t key[32];
	_aes_machine_key(key);

	const CharString utf8 = p_plain.utf8();
	const int plain_len = utf8.length();
	const int padded_len = ((plain_len / 16) + 1) * 16; // PKCS#7: always at least one pad byte.
	const uint8_t pad = (uint8_t)(padded_len - plain_len);

	Vector<uint8_t> padded;
	padded.resize(padded_len);
	memcpy(padded.ptrw(), utf8.ptr(), plain_len);
	for (int i = plain_len; i < padded_len; i++) {
		padded.write[i] = pad;
	}

	// Random IV via the shared Crypto singleton-less helper.
	Ref<Crypto> crypto = Ref<Crypto>(Crypto::create());
	PackedByteArray iv_bytes = crypto.is_valid() ? crypto->generate_random_bytes(16) : PackedByteArray();
	uint8_t iv[16];
	for (int i = 0; i < 16; i++) {
		iv[i] = i < iv_bytes.size() ? iv_bytes[i] : (uint8_t)(Math::rand() & 0xFF);
	}

	Vector<uint8_t> out;
	out.resize(16 + padded_len);
	memcpy(out.ptrw(), iv, 16); // encrypt_cbc mutates the IV; keep the original for the envelope.

	CryptoCore::AESContext aes;
	if (aes.set_encode_key(key, 256) != OK) {
		return String();
	}
	uint8_t iv_work[16];
	memcpy(iv_work, iv, 16);
	if (aes.encrypt_cbc(padded_len, iv_work, padded.ptr(), out.ptrw() + 16) != OK) {
		return String();
	}

	return String(SOLERS_SECRET_AES_PREFIX) + CryptoCore::b64_encode_str(out.ptr(), out.size());
}

String SolersSecretStore::_aes_unprotect(const String &p_payload) {
	const CharString b64 = p_payload.utf8();
	Vector<uint8_t> bytes;
	bytes.resize(b64.length());
	size_t decoded_len = 0;
	if (CryptoCore::b64_decode(bytes.ptrw(), bytes.size(), &decoded_len, (const uint8_t *)b64.ptr(), b64.length()) != OK) {
		return String();
	}
	if (decoded_len < 32 || ((decoded_len - 16) % 16) != 0) {
		return String();
	}

	uint8_t key[32];
	_aes_machine_key(key);

	CryptoCore::AESContext aes;
	if (aes.set_decode_key(key, 256) != OK) {
		return String();
	}

	const size_t ct_len = decoded_len - 16;
	Vector<uint8_t> plain;
	plain.resize(ct_len);
	uint8_t iv_work[16];
	memcpy(iv_work, bytes.ptr(), 16);
	if (aes.decrypt_cbc(ct_len, iv_work, bytes.ptr() + 16, plain.ptrw()) != OK) {
		return String();
	}

	const uint8_t pad = plain[ct_len - 1];
	if (pad == 0 || pad > 16 || pad > ct_len) {
		return String();
	}
	return String::utf8((const char *)plain.ptr(), ct_len - pad);
}

String SolersSecretStore::protect(const String &p_plain) {
	if (p_plain.is_empty() || is_protected(p_plain)) {
		return p_plain;
	}
#ifdef WINDOWS_ENABLED
	const String dpapi = _dpapi_protect(p_plain);
	if (!dpapi.is_empty()) {
		return dpapi;
	}
#endif
	const String aes = _aes_protect(p_plain);
	if (!aes.is_empty()) {
		return aes;
	}
	// Never fail the save path: storing plaintext (legacy behavior) beats
	// silently dropping the user's key.
	return p_plain;
}

String SolersSecretStore::unprotect(const String &p_stored) {
	if (p_stored.begins_with(SOLERS_SECRET_DPAPI_PREFIX)) {
#ifdef WINDOWS_ENABLED
		return _dpapi_unprotect(p_stored.substr(strlen(SOLERS_SECRET_DPAPI_PREFIX)));
#else
		return String(); // DPAPI blob from another OS is undecryptable here.
#endif
	}
	if (p_stored.begins_with(SOLERS_SECRET_AES_PREFIX)) {
		return _aes_unprotect(p_stored.substr(strlen(SOLERS_SECRET_AES_PREFIX)));
	}
	return p_stored; // Legacy plaintext.
}
