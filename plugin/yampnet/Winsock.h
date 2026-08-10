#pragma once

#include <winsock2.h>

namespace yampnet
{
	// Winsock needs process-wide startup, refcounted so opening/closing transports repeatedly
	// (host, then join, then host again) does not tear it down under a live socket.
	//
	// ONE refcount for the whole plugin (2026-08-09): this existed as two independent copies -
	// Transport's and TlsClient's - each balanced only against itself and rescued from each
	// other by Windows' own internal WSAStartup refcount. One counter says what is meant.
	inline int g_wsa_refs = 0;

	inline bool WsaAcquire()
	{
		if (g_wsa_refs++ == 0)
		{
			WSADATA data = {};
			if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			{
				g_wsa_refs = 0;
				return false;
			}
		}
		return true;
	}

	inline void WsaRelease()
	{
		if (g_wsa_refs > 0 && --g_wsa_refs == 0)
			WSACleanup();
	}
}
