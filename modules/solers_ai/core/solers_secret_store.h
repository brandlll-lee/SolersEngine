/**************************************************************************/
/*  solers_secret_store.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/*                                                                        */
/* At-rest protection for BYOK secrets stored in EditorSettings. Values   */
/* are wrapped as "sdpapi$<b64>" (Windows DPAPI, user-scoped) or          */
/* "saes1$<b64>" (AES-256-CBC keyed from the machine unique id) so the    */
/* editor settings file never carries a plaintext API key. Legacy         */
/* plaintext values pass through unprotect() unchanged, so existing       */
/* configs keep working and get re-wrapped on the next save.             */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"

class SolersSecretStore {
	static String _aes_protect(const String &p_plain);
	static String _aes_unprotect(const String &p_payload);

public:
	// Wrap a plaintext secret for storage. Returns the plaintext unchanged if
	// no protection backend is available (never fails the save path).
	static String protect(const String &p_plain);

	// Unwrap a stored value. Plaintext (legacy/unwrapped) passes through.
	static String unprotect(const String &p_stored);

	// True when the stored value carries one of the protected envelopes.
	static bool is_protected(const String &p_stored);
};
