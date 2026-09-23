// The Netplay page (account, lobby, room browser) and the in-game overlay.
// Split out of YAMPUserInterface.cpp (2026-08-09); the class and its page-copy state stay
// in YAMPUserInterface.h - this file only defines the panel methods.

#include <cstdio>
#include <cstring>

#include "../YAMPUserInterface.h"
#include "../ui/UiInternal.h"
#include "SharedLogin.h"

// UiInternal.h brings in <Windows.h> under WIN32_LEAN_AND_MEAN, which leaves this one out.
#include <shellapi.h>

namespace
{
	// Opens the Twitch verification page. Converted to wide characters rather than calling the
	// -A entry point, matching how the rest of YAMP reaches the shell (see DrawAbout).
	//
	// A failure is deliberately silent: the page always shows the code and the URL as well, so a
	// browser that would not launch costs the player a copy and paste rather than the sign-in,
	// and an error box about it would be the more confusing of the two.
	void OpenInBrowser(const char* url)
	{
		if (url == nullptr || *url == '\0')
		{
			return;
		}

		wchar_t wide[512] = {};
		const int chars = static_cast<int>(sizeof(wide) / sizeof(wide[0]));
		if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, chars) == 0)
		{
			return;
		}

		ShellExecuteW(nullptr, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL);
	}
}

// that can sit pending until the user remembers to press Apply.
void YAMPUserInterface::DrawNetplay()
{
	// A sign-up in flight is advanced from here: this page is where it can be started, it must
	// work with no game running (the launcher has no round loop to poll from), and the exchange
	// is over in a second or two. Inert when nothing is in flight.
	net::PumpAccount();

	ImGui::PushTextWrapPos();
	ImGui::TextColored(WARNING_COLOUR, "Netplay is experimental. It plays against one other machine "
		"over an RPCN server - delay-based lockstep for the fighting games (the scheme Sonic the "
		"Fighters' PS3 port used), and the game's own linked-cabinet protocol for Virtual On and "
		"Sega Racing Classic 2.");
	ImGui::PopTextWrapPos();
	ImGui::Separator();

	if (!net::IsAvailable())
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("The netplay plugin (yampnet.dll) is not loaded, so netplay is unavailable. "
			"It is an optional DLL that sits next to YAMP.exe; builds that ship without it have no netcode at all.");
		const char* why = net::LoadError();
		if (why != nullptr && *why != '\0')
		{
			ImGui::TextColored(WARNING_COLOUR, "The plugin was found but rejected: %s", why);
		}
		ImGui::PopTextWrapPos();
		return;
	}

	const net::Status status = net::GetStatus();
	// A session started with -net-server drives itself end to end (it is the two-machine
	// regression harness); letting the lobby half-steer it would only produce states neither path
	// expects, so the controls go read-only instead.
	const bool commandLineSession = net::Config().enabled;

	// ---- Netplay mode (restart required) --------------------------------------------------
	//
	// Only Virtual On currently cares, but the setting is not game-specific: it says what this
	// LAUNCH is for. A linked-cabinet game runs its second Model 2 board from boot when netplay is
	// on, and does not otherwise - an idle second cabinet is a peer the ROM waits for, and the
	// operator's menu deadlocks against it. Neither direction can be done to a running board, so
	// this takes effect on the next launch rather than immediately.
	//
	// TWO LINKED-CABINET STORIES, and the text has to match the game that is running. Virtual On
	// is two boards in ONE process, strapped by the operator before boot - hence the manual
	// cabinet combo and the restart warnings. Sega Racing Classic 2 is one board per MACHINE and
	// the ROOM decides which cabinet each machine is (host = MASTER, guest = SLAVE), with the
	// board rebooted into its role when the room forms - see CommBoard::DriveRoomRole. Showing
	// Virtual On's strapping UI during an SRC2 session therefore described a mechanism the game
	// does not have, in another game's name.
	const bool src2Running = gGeneral.GetGameId() == YAMPGeneral::GameId::SRC2;
	{
		if (ImGui::Checkbox("Enable netplay for this game (restart required)", &m_netEnabled))
		{
			m_pageModified = true;
		}
		ImGui::PushTextWrapPos();
		if (src2Running)
		{
			ImGui::TextDisabled("Sega Racing Classic 2 is a LINKED-CABINET game: each machine runs "
				"one cabinet and the game's own comm-board protocol links them over the room. The "
				"room decides which cabinet this machine is - hosting makes it the MASTER, joining "
				"makes it the SLAVE - and the board reboots itself into that role when the room "
				"forms, exactly as if the operator had re-strapped a real cabinet and power-cycled "
				"it. There is nothing to configure here beforehand.");
		}
		else
		{
			ImGui::TextDisabled("Virtual On is a LINKED-CABINET game: with this off it runs a single "
				"cabinet, which is what local play and the operator's Test menu need. Turning it on "
				"brings the second board up from boot, which is required for a match and cannot be "
				"done to a board that is already running.");
		}
		if (m_netEnabled != net::WantsNetplayBoards())
		{
			ImGui::TextColored(WARNING_COLOUR, "Restart YAMP for this to take effect - the board "
				"count was decided when the game started.");
		}
		ImGui::PopTextWrapPos();

		// ---- Cabinet role (Virtual On, restart required) ----------------------------------
		//
		// Deliberately NOT hidden when another game is running: it is a property of this MACHINE,
		// not of the running session, and hiding it would mean it could only be set while Virtual
		// On was already up - i.e. only after the boot that reads it. The ONE exception is a
		// running SRC2 session: its role comes from the room (above), every control in this block
		// is consumed by Virtual On's host alone, and a MASTER/SLAVE combo under another game's
		// name next to "the room decides" reads as a contradiction, not a preset.
		if (!src2Running)
		{
			static const char* const labels[] = { "No link (standalone)", "MASTER", "SLAVE" };
			if (m_vonCabinetRole >= std::size(labels))
			{
				m_vonCabinetRole = 0;
			}
			if (ImGui::BeginCombo("Virtual On cabinet (restart required)", labels[m_vonCabinetRole]))
			{
				for (uint32_t index = 0; index < std::size(labels); index++)
				{
					const bool isSelected = index == m_vonCabinetRole;
					if (ImGui::Selectable(labels[index], isSelected))
					{
						m_vonCabinetRole = index;
						m_pageModified = true;
					}
					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::PushTextWrapPos();
			ImGui::TextDisabled("On real hardware two cabinets are wired together and each is "
				"strapped as the master or the slave site. Pick one here and the other on the "
				"second machine; the ROM then runs its own link check at boot and prints THIS IS "
				"MASTER SITE / SLAVE SITE instead of coming up standalone. \"No link\" is what the "
				"module does on its own.");
			ImGui::TextDisabled("This is the CABINET's identity, not the player's - it says which "
				"end of the cable this machine is, and it is read once during boot.");
			ImGui::PopTextWrapPos();

			if (ImGui::Checkbox("Log linked-cabinet state", &m_vonLinkLog))
			{
				m_pageModified = true;
			}
			ImGui::PushTextWrapPos();
			ImGui::TextDisabled("Writes both boards' link ID, network flag, mode and frame counter "
				"to the log every 200 frames - which is how you tell whether the cabinet above "
				"actually took and whether the two machines linked. A setting rather than a "
				"command-line switch on purpose: the game launcher cannot pass switches, so a "
				"launcher-started run could never turn this on.");
			ImGui::PopTextWrapPos();

			if (ImGui::Checkbox("Wait for the other cabinet at boot", &m_vonHoldLink))
			{
				m_pageModified = true;
			}
			ImGui::PushTextWrapPos();
			ImGui::TextDisabled("The emulated comm board reports a healthy two-cabinet ring even "
				"when nothing is connected, so the boot-time link check always succeeds. This "
				"reports the truth instead, and the game does what a real cabinet does when its "
				"partner is not switched on yet: it holds on \"Checking Network Now\" until the "
				"other end answers.");
			ImGui::TextColored(WARNING_COLOUR, "Nothing releases it yet, so the game will wait "
				"there indefinitely. This is for testing the handshake.");
			ImGui::PopTextWrapPos();
		}
		ImGui::Separator();
	}

	// ---- Account ------------------------------------------------------------------------------
	ImGui::TextUnformatted("Account");
	ImGui::Separator();

	// Consumed at Connect, so these stay editable right up to that point and never need a restart.
	// Read-only rather than hidden once a session is up: the values still have to be readable, and
	// this ImGui build has no BeginDisabled. FAILED counts as editable on purpose — a rejected
	// login is exactly when a credential needs correcting.
	const bool accountLocked = commandLineSession
		|| (status.state != YAMPNET_STATE_IDLE && status.state != YAMPNET_STATE_FAILED);
	const ImGuiInputTextFlags lockFlag = accountLocked ? ImGuiInputTextFlags_ReadOnly : 0;
	if (accountLocked)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
	}

	ImGui::PushItemWidth(-180.0f);
	if (ImGui::InputText("Server", m_netServer, sizeof(m_netServer), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Host name or address of the RPCN server. Both players must use the same one.\n"
			"The list below fills in either of the two public ones.");
	}

	// The two public servers, as m2-hle2's sign-in screen offers them. A preset rather than a
	// replacement for the box: a private or local server is still typed in. Picking one also
	// empties the certificate pin, because neither needs one (ours has a real certificate, and a
	// current plugin carries the official one's pin) and a pin left over from the other server
	// would only make the connection fail.
	{
		struct Preset { const char* host; const char* label; };
		static const Preset presets[] = {
			{ YAMPSettings::kDefaultRpcnServer, "rpcn.sonicthefighte.rs (YAMP's server)" },
			{ YAMPSettings::kOfficialRpcnServer, "np.rpcs3.net (the official RPCN server)" },
		};
		const char* preview = "Other (typed above)";
		for (const Preset& preset : presets)
		{
			if (_stricmp(m_netServer, preset.host) == 0)
			{
				preview = preset.label;
			}
		}
		if (!accountLocked && ImGui::BeginCombo("Public servers", preview))
		{
			for (const Preset& preset : presets)
			{
				const bool isSelected = _stricmp(m_netServer, preset.host) == 0;
				if (ImGui::Selectable(preset.label, isSelected) && !isSelected)
				{
					strncpy_s(m_netServer, sizeof(m_netServer), preset.host, _TRUNCATE);
					m_netFingerprint[0] = '\0';
					m_pageModified = true;
				}
				if (isSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		else if (accountLocked)
		{
			ImGui::LabelText("Public servers", "%s", preview);
		}
	}
	ImGui::PushTextWrapPos();
	// Said up front because nothing else on the page would: the boxes below look like they
	// describe "the player", and switching Server does not bring the account along.
	ImGui::TextDisabled("Accounts belong to one server. An account made on YAMP's server does "
		"not exist on np.rpcs3.net, and the other way round - \"Create account\" below makes it "
		"on whichever server is selected. np.rpcs3.net has no Twitch sign-in.");
	ImGui::PopTextWrapPos();

	if (ImGui::InputText("Account (NPID)", m_netNpid, sizeof(m_netNpid), lockFlag)) m_pageModified = true;

	const ImGuiInputTextFlags passwordFlags =
		lockFlag | (m_netShowPassword ? 0 : ImGuiInputTextFlags_Password);
	if (ImGui::InputText("Password", m_netPassword, sizeof(m_netPassword), passwordFlags)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Stored in plain text in the settings file, like every other setting.\n"
			"Use an account you do not mind being readable there.\n"
			"\n"
			"Can stay empty while a Twitch sign-in for this server and account is saved.");
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::Checkbox("Show", &m_netShowPassword);

	// The saved Twitch sign-in, which is kept beside the password rather than in it and goes
	// only to the server that issued it (YAMPSettings). Said on the page because it decides what
	// Connect sends, and because "why is my Twitch login not working on np.rpcs3.net" has exactly
	// one answer.
	//
	// Asked through net::TwitchLoginFor, the same decision Connect makes, which includes the
	// login m2-hle2 shares: a page that looked only at YAMP's own copy said "no Twitch sign-in"
	// and then Connect offered the shared one anyway.
	const net::SavedTwitch savedTwitch = { m_netTwitchToken, m_netTwitchNpid, m_netTwitchServer };
	const net::TwitchSource twitchSource = net::TwitchLoginFor(m_netServer, m_netNpid, &savedTwitch);
	if (twitchSource != net::TwitchSource::None || m_netTwitchToken[0] != '\0')
	{
		ImGui::PushTextWrapPos();
		if (twitchSource == net::TwitchSource::Shared)
		{
			ImGui::TextDisabled("Signed in with Twitch as %s on %s, with the login shared with "
				"m2-hle2 on this machine, so the password can stay empty.", m_netNpid, m_netServer);
		}
		else if (twitchSource == net::TwitchSource::Saved)
		{
			ImGui::TextDisabled("Signed in with Twitch as %s on %s, so the password can stay "
				"empty.", m_netTwitchNpid, m_netTwitchServer);
		}
		else
		{
			ImGui::TextDisabled("A Twitch sign-in is saved for %s on %s. It is only ever sent "
				"there, so it is not used for this server and account.", m_netTwitchNpid,
				m_netTwitchServer);
		}
		ImGui::PopTextWrapPos();
		if (!accountLocked)
		{
			if (ImGui::Button("Forget the Twitch sign-in"))
			{
				// Both copies, or Forget would not stop the login it names: the shared one is
				// offered first. Only the shared token for THIS server and account is touched.
				if (twitchSource != net::TwitchSource::None)
				{
					net::ForgetSharedTwitchLogin(m_netServer, m_netNpid);
				}
				m_netTwitchToken[0] = m_netTwitchNpid[0] = m_netTwitchServer[0] = '\0';
				m_pageModified = true;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Also removes it from the login m2-hle2 shares on this machine, so\n"
					"m2-hle2 has to sign in with Twitch again as well. The saved settings\n"
					"change when you press Apply; the shared login goes at once.");
			}
		}
	}

	// The verification token. A SEPARATE thing from the password above, and empty for almost
	// everyone: only a server that validates accounts by e-mail ever checks it. It gets a box
	// of its own rather than a "having trouble?" link because a player who needs one cannot
	// log in at all until it is filled in, and the login error names this field.
	ImGui::PushItemWidth(-180.0f);
	if (ImGui::InputTextWithHint("Verification token", "usually empty", m_netToken,
		sizeof(m_netToken), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("LEAVE IT EMPTY unless the server asked for one. This is not a second\n"
			"password: it is the 16-character code some servers e-mail when an account\n"
			"is created, and only those servers check it.\n"
			"\n"
			"If yours does, Connect fails saying the token is missing or wrong - that\n"
			"is when to paste it here. The sign-up section below can have a fresh copy\n"
			"e-mailed if the message never arrived.");
	}
	ImGui::PopItemWidth();

	ImGui::PushItemWidth(-180.0f);
	// Empty is the normal state, so the hint has to say what empty DOES - a blank box that
	// silently picks something is worse than no box at all. It names the actual game rather than
	// "automatic" so the player can see which lobby space they are about to be in.
	char comIdHint[96] = {};
	snprintf(comIdHint, sizeof(comIdHint), "automatic - %s", net::AutoComIdKey());
	if (ImGui::InputTextWithHint("Communication ID", comIdHint, m_netComId, sizeof(m_netComId),
		lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Which lobby space the rooms live in. Both players must match.\n"
			"\n"
			"LEAVE IT EMPTY. Each game then gets a space of its own, so a room list only\n"
			"ever shows matches you can actually play - which is what a blank box means\n"
			"here, not that nothing is set.\n"
			"\n"
			"Fill it in only to meet someone outside that: a game's NAME works as well as\n"
			"a literal comm id (9 uppercase letters/digits, '_', 2 digits), and the server\n"
			"registers an unknown one on first use.");
	}

	if (ImGui::InputText("Certificate SHA-256", m_netFingerprint, sizeof(m_netFingerprint), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Leave EMPTY for a server with a proper certificate - it is then validated\n"
			"the way a browser validates a website, and nothing needs to be pasted here.\n"
			"\n"
			"Fill it in only for a SELF-SIGNED server: those certificates carry no usable name, so\n"
			"ordinary validation can never accept them and the exact certificate is pinned instead.\n"
			"Connecting to one unpinned fails with its fingerprint in the message (and in\n"
			"yampnet.log) - that is the value to paste here.\n"
			"\n"
			"np.rpcs3.net is self-signed too, but a current netplay plugin has its pin built in,\n"
			"so it needs nothing here either. An older plugin needs it pasted:\n"
			"7028AD2117139EEA9E1FE14713D0CB4FCD8815B2F39AB4E10FF6725A38AFD155\n"
			"\n"
			"Do not pin a real certificate: it is reissued every renewal and the pin would then\n"
			"start rejecting the server.");
	}

	// ---- Sign in with Twitch ------------------------------------------------------------------
	//
	// The other way to have an account here, and the one that asks nothing of a player who has
	// never heard of RPCN: the server runs an OAuth device code flow against Twitch and answers
	// with an npid and a login token. YAMP never sees a Twitch password - only a short code to
	// put on screen and a page to open.
	//
	// It sits BELOW the credential boxes because what it does is fill them in, and ABOVE the
	// sign-up because it is the shorter road for most people: the two are alternatives, and the
	// one that needs no e-mail address should be read first.
	if (!accountLocked)
	{
		const yampnet_twitch_state twitchState = net::TwitchState();
		net::TwitchInfo twitch;
		net::TwitchGetInfo(&twitch);

		const bool twitchBusy = twitchState == YAMPNET_TWITCH_STARTING
			|| twitchState == YAMPNET_TWITCH_WAITING;
		// Forced open while a flow is live. The code on screen is the only way to finish one, and
		// a collapsed header would hide it behind a click nothing tells the player to make.
		if (twitchBusy)
		{
			ImGui::SetNextItemOpen(true);
		}

		if (ImGui::CollapsingHeader("Sign in with Twitch instead"))
		{
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted("Uses a Twitch account, so there is no account here to make or "
				"remember. The server does the talking to Twitch - YAMP never sees a Twitch password - "
				"and answers with an account name and a login token. The token is saved with the "
				"server that issued it and is only ever sent there.");
			ImGui::PopTextWrapPos();

			switch (twitchState)
			{
			case YAMPNET_TWITCH_STARTING:
				ImGui::TextDisabled("Asking the server for a code...");
				break;

			case YAMPNET_TWITCH_WAITING:
			{
				// Opened once, when the code first exists. The URL already carries the code, so this
				// is the whole of what the player has to do - but the code is shown regardless,
				// because a browser that did not launch leaves nothing else to go on, and because the
				// page should be checked to be asking about THIS code rather than an abandoned one.
				if (!m_netTwitchOpened)
				{
					m_netTwitchOpened = true;
					OpenInBrowser(twitch.verification_uri);
				}

				ImGui::TextUnformatted("Enter this code on the Twitch page:");
				ImGui::TextColored(WARNING_COLOUR, "%s", twitch.user_code);
				ImGui::TextDisabled("%s", twitch.verification_uri);

				if (ImGui::Button("Open the page again"))
				{
					OpenInBrowser(twitch.verification_uri);
				}
				ImGui::SameLine();
				// NOT plain "Cancel": the settings window has one of those, and two buttons with the
				// same label in one window are the same button as far as ImGui is concerned.
				if (ImGui::Button("Cancel the sign-in"))
				{
					net::TwitchCancel();
				}

				ImGui::Text("The code expires in %u:%02u.", twitch.seconds_remaining / 60u,
					twitch.seconds_remaining % 60u);
				ImGui::PushTextWrapPos();
				// Worth saying because it is not guessable: the sign-in is driven from this page, so
				// a player who closes the settings window while authorising comes back to a flow the
				// server has since dropped.
				ImGui::TextUnformatted("Leave this page open while you do it - the sign-in only "
					"advances while it is on screen.");
				ImGui::PopTextWrapPos();
				break;
			}

			case YAMPNET_TWITCH_DONE:
			{
				// Copied ONCE. The plugin holds DONE until the sign-in is cancelled, so doing this
				// every frame would keep overwriting boxes the player had since corrected.
				if (!m_netTwitchCaptured)
				{
					m_netTwitchCaptured = true;
					// Only the button above starts a flow and it always records the server; this
					// is for a DONE that outlived the page's state, never the normal road.
					if (m_netTwitchFlowServer[0] == '\0')
					{
						strncpy_s(m_netTwitchFlowServer, sizeof(m_netTwitchFlowServer), m_netServer,
							_TRUNCATE);
					}
					// A password typed for another account means nothing to this one. One for the
					// SAME account is kept: RPCN links Twitch to an account without retiring its
					// password, so that still works as the fallback.
					if (_stricmp(m_netNpid, twitch.npid) != 0)
					{
						m_netPassword[0] = '\0';
					}
					strncpy_s(m_netNpid, sizeof(m_netNpid), twitch.npid, _TRUNCATE);
					// The login token is kept APART from the password, with the server that issued
					// it: it is a password to that server and to no other (YAMPSettings). The server
					// is the one the flow was started on, not whatever the Server box says now.
					strncpy_s(m_netTwitchToken, sizeof(m_netTwitchToken), twitch.login_token, _TRUNCATE);
					strncpy_s(m_netTwitchNpid, sizeof(m_netTwitchNpid), twitch.npid, _TRUNCATE);
					strncpy_s(m_netTwitchServer, sizeof(m_netTwitchServer), m_netTwitchFlowServer,
						_TRUNCATE);
					// The verification token is cleared because it plays no part in a Twitch login:
					// a value left over from another account would only be a puzzle the next time a
					// login is refused.
					m_netToken[0] = '\0';
					m_pageModified = true;
					// And into the file m2-hle2 signs in from (SharedLogin.h), straight away and not
					// on Apply: this token has just retired the one in that file, so leaving it
					// there would sign m2-hle2 out.
					if (!net::StoreSharedTwitchToken(m_netTwitchFlowServer, twitch.npid,
							twitch.login_token))
						net::Logf("could not share the Twitch login with m2-hle2 (%ls)\n",
							net::SharedLoginPath().c_str());
				}

				ImGui::PushTextWrapPos();
				ImGui::Text("Signed in as %s. The account is %s on %s, and it needs no password "
					"there.", twitch.online_name, twitch.npid, m_netTwitchFlowServer);
				// The warning is not decoration. A finished sign-in REPLACES the account's previous
				// token, so one that is thrown away by Cancel cannot be recovered - it has to be
				// done again, and whatever was in the password box before is dead either way.
				ImGui::TextColored(WARNING_COLOUR, "Press Apply to save it. Signing in again issues a "
					"fresh token and stops this one working, so an unsaved one has to be done over.");
				ImGui::PopTextWrapPos();

				// Without this the section is a dead end: the plugin holds DONE until something
				// clears it, so a player who signed in as the wrong Twitch account had no way back
				// to the button short of restarting YAMP. Cancelling a FINISHED flow only drops the
				// plugin's copy of it - the sign-in is already saved on the page.
				if (ImGui::Button("Sign in as someone else"))
				{
					net::TwitchCancel();
				}
				break;
			}

			default:
			{
				// IDLE, FAILED and UNSUPPORTED all end up here: in each the only thing to offer is
				// the button, and the difference between them is what is written under it.
				const bool ready = m_netServer[0] != '\0';
				// np.rpcs3.net has no Twitch sign-in, so the flow is run on ours instead, as
				// m2-hle2's lobby does. Any other typed-in server is tried as it is: a private one
				// may offer it, and the plugin says plainly when one does not.
				const bool onOfficial = _stricmp(m_netServer, YAMPSettings::kOfficialRpcnServer) == 0;
				const char* label = (twitchState == YAMPNET_TWITCH_IDLE) ? "Sign in with Twitch"
					: "Try Twitch sign-in again";
				if (ImGuiCustom::ButtonToggleable(label, ready))
				{
					if (onOfficial)
					{
						strncpy_s(m_netServer, sizeof(m_netServer), YAMPSettings::kDefaultRpcnServer,
							_TRUNCATE);
						m_netFingerprint[0] = '\0';   // the official server's pin is not ours
						m_pageModified = true;
					}
					m_netTwitchCaptured = false;
					m_netTwitchOpened = false;
					strncpy_s(m_netTwitchFlowServer, sizeof(m_netTwitchFlowServer), m_netServer,
						_TRUNCATE);
					net::TwitchLogin(m_netServer, m_netFingerprint);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(!ready
						? "Fill in the server first."
						: onOfficial
						? "np.rpcs3.net has no Twitch sign-in, so this switches Server to\n"
						  "rpcn.sonicthefighte.rs first. A Twitch page then opens in the web\n"
						  "browser with the code already filled in."
						: "A Twitch page opens in the web browser with the code already filled in.\n"
						  "Nothing is typed unless the browser fails to open.");
				}

				if (twitchState == YAMPNET_TWITCH_UNSUPPORTED)
				{
					ImGui::PushTextWrapPos();
					// Deliberately NOT the warning colour. Nobody did anything wrong and nothing on
					// this page needs correcting - this server simply does not offer it.
					ImGui::Text("%s.", net::TwitchError());
					// The button above is still worth having, but only for the one thing that can
					// change the answer, so the text says what that is rather than leaving a "try
					// again" that would fail the same way every time.
					ImGui::TextUnformatted("Use an account and password instead - the section below "
						"can register one - or point Server at one that does offer it and try again.");
					ImGui::PopTextWrapPos();
				}
				else if (twitchState == YAMPNET_TWITCH_FAILED)
				{
					ImGui::PushTextWrapPos();
					ImGui::TextColored(WARNING_COLOUR, "%s", net::TwitchError());
					ImGui::PopTextWrapPos();
				}
				break;
			}
			}
		}
	}

	// ---- Create an account --------------------------------------------------------------------
	//
	// An RPCN account had to be made with some other client before any of this was usable, which
	// is a strange first step for a player whose only PlayStation-anything is this emulator. It
	// reuses the name and password typed above rather than asking for them twice: what is being
	// registered IS the account this page will log in with.
	//
	// The token resend lives here too, because it is the same account being made and the same
	// e-mail that has to arrive: on a server that verifies accounts, a sign-up whose message got
	// lost leaves an account that can never log in, and this is where its owner is standing.
	if (!accountLocked && ImGui::CollapsingHeader("Create a new account, or re-send its token"))
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("Registers the account name and password above on the server above. The e-mail address is stored by the server and is not saved in your settings.");
		ImGui::Text("The account exists only on %s. Switching Server later means an account there as well.",
			m_netServer[0] != '\0' ? m_netServer : "the selected server");
		ImGui::TextUnformatted("Some servers then e-mail a verification token that the account cannot log in without. If one never arrives, ask for another below - that needs no e-mail address, only the account and password above.");
		ImGui::PopTextWrapPos();

		ImGui::PushItemWidth(-180.0f);
		// Not m_pageModified: this is never saved, so there is nothing pending to Apply.
		ImGui::InputText("E-mail", m_netEmail, sizeof(m_netEmail));
		ImGui::PopItemWidth();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The server stores one for every account and rejects an address that\n"
				"cannot be a real one. Whether it ever gets mail depends on the server.");
		}

		const yampnet_account_state acct = net::AccountState();
		const bool ready = m_netServer[0] != '\0' && m_netNpid[0] != '\0'
			&& m_netPassword[0] != '\0' && m_netEmail[0] != '\0'
			&& acct != YAMPNET_ACCOUNT_WORKING;
		if (ImGuiCustom::ButtonToggleable("Create account", ready))
		{
			net::CreateAccount(m_netServer, m_netNpid, m_netPassword, m_netEmail, m_netFingerprint);
		}
		if (!ready && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Fill in the server, account, password and e-mail first.");
		}

		// Only the account name and password are needed to ask for a token - that is the whole
		// point of the command, since a player who lost the e-mail has no token to prove anything
		// with. It sits under the sign-up header because this is where an account is made, and a
		// message that never arrives is the very next thing that can go wrong.
		const bool resendReady = m_netServer[0] != '\0' && m_netNpid[0] != '\0'
			&& m_netPassword[0] != '\0' && acct != YAMPNET_ACCOUNT_WORKING;
		if (ImGuiCustom::ButtonToggleable("Have the token e-mailed again", resendReady))
		{
			net::ResendToken(m_netServer, m_netNpid, m_netPassword, m_netFingerprint);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("For an account that already exists on this server and never got its\n"
				"verification e-mail. Uses the account and password above - no token is\n"
				"needed to ask for the token, which is the point.\n"
				"\n"
				"A server that does not verify accounts by e-mail has nothing to send and\n"
				"says so. One e-mail per account per 24 hours.");
		}

		switch (acct)
		{
		case YAMPNET_ACCOUNT_WORKING:
			ImGui::TextDisabled("Talking to the server...");
			break;
		case YAMPNET_ACCOUNT_CREATED:
			ImGui::PushTextWrapPos();
			// Says to look for the e-mail without promising one: whether a token is mailed at all
			// is the server's setting, and the plugin is never told which sort it just used.
			ImGui::TextUnformatted("Account created. Press Connect to use it - and if the server "
				"e-mails a verification token, paste that into the token box above first.");
			ImGui::PopTextWrapPos();
			break;
		case YAMPNET_ACCOUNT_TOKEN_SENT:
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted("Token sent. Check the e-mail the account was registered with, "
				"then paste the code into the token box above.");
			ImGui::PopTextWrapPos();
			break;
		case YAMPNET_ACCOUNT_FAILED:
			ImGui::PushTextWrapPos();
			ImGui::TextColored(WARNING_COLOUR, "%s", net::AccountError());
			ImGui::PopTextWrapPos();
			break;
		default:
			break;
		}
	}

	// The delay is read when a round starts, not when the session connects, so it stays editable
	// between matches — but not while one is running, where it would silently mean nothing.
	if (status.state == YAMPNET_STATE_SYNCING || status.state == YAMPNET_STATE_IN_MATCH)
	{
		ImGui::LabelText("Input delay", "%d frames", m_netFrameDelay);
	}
	else if (ImGui::SliderInt("Input delay", &m_netFrameDelay, 0, 10, "%d frames"))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Frames of delay applied to both players' inputs to hide network latency.\n"
			"Too low for the connection and the game stalls rather than desyncing; 2-4 suits most links.\n"
			"Read when a match starts, so a change applies to the next one.");
	}
	ImGui::PopItemWidth();

	// Which state a ROUND starts from, on the boards that have a choice. Read when the room is
	// created, so it is greyed out once one exists: the room owns the value from then on, and a
	// control that silently stopped mattering would be worse than one that says so.
	if (CurrentRoomSetting() == RoomSetting::Pre3Start)
	{
		const bool roomExists = status.state == YAMPNET_STATE_IN_ROOM
			|| status.state == YAMPNET_STATE_SYNCING
			|| status.state == YAMPNET_STATE_IN_MATCH;
		if (roomExists)
		{
			ImGui::LabelText("Rounds start",
				status.pre3_vs_start ? "in a versus match" : "at power-on");
		}
		else if (ImGui::Checkbox("Start rounds in a versus match", &m_netPre3VsStart))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				"Both machines restore the same saved board when a round starts, so this is a\n"
				"property of the ROOM - the host's choice, adopted by whoever joins.\n\n"
				"Off: the board's POWER-ON state. The round plays forward through the boot, the\n"
				"attract demo and the credit screen - which is where the AI runs, and so where a\n"
				"divergence between the two machines shows itself.\n\n"
				"On: the versus start state the board ships. Straight into a match, no boot to\n"
				"sit through.");
		}
	}

	if (accountLocked)
	{
		ImGui::PopStyleVar();
		ImGui::TextDisabled(commandLineSession
			? "Driven by the command line for this session."
			: "Disconnect to change these.");
	}

	// ---- Lobby --------------------------------------------------------------------------------
	ImGui::NewLine();
	ImGui::TextUnformatted("Session");
	ImGui::Separator();

	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted(status.text);
	if (status.error != nullptr && *status.error != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", status.error);
	}
	// Latched for the rest of the session on purpose: this is the single most useful thing the
	// netplay UI can tell you, and it must not scroll away with the next status change.
	if (status.desynced)
	{
		ImGui::TextColored(WARNING_COLOUR,
			"Desync detected at frame %u (this machine %u, the other %u). The two emulators "
			"stopped simulating the same game there; see yampnet.log.",
			status.desync_frame, status.desync_local, status.desync_remote);
	}
	if (const char* actionError = net::LastActionError(); actionError != nullptr && *actionError != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", actionError);
	}
	ImGui::PopTextWrapPos();

	if (commandLineSession)
	{
		ImGui::TextDisabled("Started from the command line; the lobby controls are disabled.");
		return;
	}

	switch (status.state)
	{
	case YAMPNET_STATE_IDLE:
	case YAMPNET_STATE_FAILED:
	{
		// The token is NOT part of this test: empty is its normal value, and a server that wants
		// one says so when it refuses the login.
		// A Twitch sign-in stands in for the password, but only on its own server - asked the same
		// way Connect decides, shared login included.
		const bool twitchHere = twitchSource != net::TwitchSource::None;
		const bool ready = m_netServer[0] != '\0' && m_netNpid[0] != '\0'
			&& (m_netPassword[0] != '\0' || twitchHere);
		if (ImGuiCustom::ButtonToggleable("Connect", ready))
		{
			// Deliberately the page's live buffers rather than the saved settings: connecting is
			// how you find out a credential is wrong, and having to Apply first would make fixing
			// it a two-step dance.
			net::Connect(m_netServer, m_netNpid, m_netPassword, m_netToken, m_netFingerprint,
				m_netComId, &savedTwitch);
		}
		if (!ready && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(m_netTwitchToken[0] != '\0'
				? "Fill in the server, account and password first.\n"
				  "The saved Twitch sign-in is only sent to the server that issued it."
				: "Fill in the server, account and password first.");
		}
		break;
	}

	case YAMPNET_STATE_CONNECTING:
		if (ImGui::Button("Cancel"))
		{
			net::Disconnect();
		}
		break;

	case YAMPNET_STATE_ONLINE:
	{
		if (ImGui::Button("Host a room"))
		{
			// The room takes this machine's cabinet settings with it. Read from the SAVED
			// settings, not the Game page's edit buffer: an unapplied combo change would
			// otherwise publish a value the local emulator is not running under.
			const YAMPSettings* set = gGeneral.GetSettings();
			// SRC2's five GAME ASSIGNMENTS come from the BOARD's working copy, not from any
			// settings buffer: it is the one place that already folds together the injector,
			// YAMP's writes and the operator's own service-menu edits. present stays false when
			// the board is not up (or the game is not SRC2), which publishes nothing.
			net::Src2Assignments src2 = {};
			pre3::ArcadeSettings::LiveAssignments live = {};
			if (pre3::ArcadeSettings::ReadLiveAssignments(live))
			{
				src2.present = true;
				src2.cabinetType = live.cabinetType;
				src2.difficulty = live.difficulty;
				src2.gameMode = live.gameMode;
				src2.motorPower = live.motorPower;
				src2.ranking = live.ranking;
			}
			net::HostRoom(m_netRoomPassword, set != nullptr && set->m_m2RealDamage,
				set != nullptr && set->m_vf2Version20,
				set != nullptr && set->m_m2VersusMode,
				set != nullptr && set->m_netPre3VsStart,
				&src2);
		}
		if (ImGui::IsItemHovered())
		{
			if (CurrentRoomSetting() == RoomSetting::Src2Assign)
			{
				ImGui::SetTooltip("The room is created with this cabinet's current GAME ASSIGNMENTS\n"
					"(difficulty, game mode, motor power, cabinet type, ranking mode) so the room\n"
					"list shows what the race is. Set them in the service menu before hosting.");
			}
			else
			{
				ImGui::SetTooltip("The room is created with your current Damage setting (Game page),\n"
					"and everyone who joins plays under it. It cannot be changed once the room exists.");
			}
		}
		ImGui::SameLine();
		ImGui::PushItemWidth(160.0f);
		ImGui::InputText("Room password", m_netRoomPassword, sizeof(m_netRoomPassword));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Optional. Leave empty for a room anyone with the ID can join.");
		}

		ImGui::PopItemWidth();

		// ---- Room browser --------------------------------------------------------------------
		ImGui::NewLine();
		if (ImGui::Button("Refresh room list"))
		{
			net::RefreshRooms();
			m_netSelectedRoom = 0;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Rooms are listed by the account hosting them.");

		net::RoomRow rooms[16];
		const unsigned int roomCount = net::GetRooms(rooms, static_cast<unsigned int>(std::size(rooms)));

		// Only the columns this game actually has. A room publishes at most one per-game setting
		// and not every game has one, so the table is built from CurrentRoomSetting rather than
		// from "is it VF2?" - which is what used to show Fighting Vipers and the Model 3 boards a
		// Damage column that nothing on either side reads.
		const RoomSetting roomSetting = CurrentRoomSetting();
		const bool showVs = RoomHasVsMode();
		// SRC2 publishes five assignment fields where the other games publish one setting.
		const int settingColumns = roomSetting == RoomSetting::Src2Assign ? 5
			: roomSetting != RoomSetting::None ? 1 : 0;
		const int roomColumns = 4 + settingColumns + (showVs ? 1 : 0);

		// SRC2's five assignment columns overflow the page width; a horizontal scroll keeps every
		// column readable where the default sizing would squeeze them all illegible. ScrollX and
		// stretch columns fight each other (a stretch column absorbs exactly the width the scroll
		// exists to provide), so the Host column goes fixed on the wide table.
		const bool wideTable = roomSetting == RoomSetting::Src2Assign;
		if (ImGui::BeginTable("##rooms", roomColumns,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
				| (wideTable ? ImGuiTableFlags_ScrollX : 0),
			{ 0.0f, 130.0f }))
		{
			if (wideTable)
			{
				ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthFixed, 140.0f);
			}
			else
			{
				ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
			}
			ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			switch (roomSetting)
			{
			case RoomSetting::Damage:
				ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				break;
			case RoomSetting::Vf2Version:
				ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				break;
			case RoomSetting::Pre3Start:
				ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				break;
			case RoomSetting::Src2Assign:
				ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthFixed, 65.0f);
				ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Motor", ImGuiTableColumnFlags_WidthFixed, 50.0f);
				ImGui::TableSetupColumn("Cabinet", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("Ranking", ImGuiTableColumnFlags_WidthFixed, 65.0f);
				break;
			case RoomSetting::None:
				break;
			}
			if (showVs)
			{
				ImGui::TableSetupColumn("VS", ImGuiTableColumnFlags_WidthFixed, 40.0f);
			}
			ImGui::TableSetupColumn("Locked", ImGuiTableColumnFlags_WidthFixed, 55.0f);
			ImGui::TableSetupColumn("Room ID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableHeadersRow();

			for (unsigned int i = 0; i < roomCount; i++)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(static_cast<int>(i));
				const bool selected = m_netSelectedRoom == rooms[i].room_id;
				if (ImGui::Selectable(rooms[i].owner, selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_netSelectedRoom = rooms[i].room_id;
					sprintf_s(m_netJoinRoomId, "%llu", rooms[i].room_id);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u/%u", rooms[i].players, rooms[i].max_players);
				// The host's published room setting, if this game has one. Worth a column of its
				// own because it is not a preference you keep on joining - it is how that match
				// will play.
				int col = 2;
				switch (roomSetting)
				{
				case RoomSetting::Damage:
					ImGui::TableSetColumnIndex(col++);
					ImGui::TextUnformatted(rooms[i].real_damage ? "Real" : "Normal");
					break;
				case RoomSetting::Vf2Version:
					// 2.0 and 2.1 are mechanically different games, not a tuning option.
					ImGui::TableSetColumnIndex(col++);
					ImGui::TextUnformatted(rooms[i].vf2_version20 ? "2.0" : "2.1");
					break;
				case RoomSetting::Src2Assign:
					// Older hosts publish no assignments; "?" says so rather than showing the
					// all-zero decode as a real Deluxe/Easy/Sprint room. The bounds check exists
					// because the wire fields are 2-3 BITS and two of the option lists have fewer
					// entries than the field can express - a hand-crafted flagAttr must degrade to
					// "?", not index past a label table.
					if (rooms[i].src2.present)
					{
						const auto name = [](const char* const* names, size_t count, uint8_t v)
						{
							return v < count ? names[v] : "?";
						};
						ImGui::TableSetColumnIndex(col++);
						ImGui::TextUnformatted(name(SRC2_DIFFICULTY_NAMES,
							std::size(SRC2_DIFFICULTY_NAMES), rooms[i].src2.difficulty));
						ImGui::TableSetColumnIndex(col++);
						ImGui::TextUnformatted(name(SRC2_MODE_NAMES,
							std::size(SRC2_MODE_NAMES), rooms[i].src2.gameMode));
						ImGui::TableSetColumnIndex(col++);
						ImGui::TextUnformatted(name(SRC2_MOTOR_NAMES,
							std::size(SRC2_MOTOR_NAMES), rooms[i].src2.motorPower));
						ImGui::TableSetColumnIndex(col++);
						ImGui::TextUnformatted(name(SRC2_CABINET_NAMES,
							std::size(SRC2_CABINET_NAMES), rooms[i].src2.cabinetType));
						ImGui::TableSetColumnIndex(col++);
						ImGui::TextUnformatted(name(SRC2_RANKING_NAMES,
							std::size(SRC2_RANKING_NAMES), rooms[i].src2.ranking));
					}
					else
					{
						for (int field = 0; field < 5; field++)
						{
							ImGui::TableSetColumnIndex(col++);
							ImGui::TextDisabled("?");
						}
					}
					break;
				case RoomSetting::Pre3Start:
					ImGui::TableSetColumnIndex(col++);
					ImGui::TextUnformatted(rooms[i].pre3_vs_start ? "Versus" : "Power-on");
					break;
				case RoomSetting::None:
					break;
				}
				if (showVs)
				{
					ImGui::TableSetColumnIndex(col++);
					// Blank rather than "no" when off, so a browser full of ordinary arcade rooms
					// stays quiet and the VS ones stand out.
					ImGui::TextUnformatted(rooms[i].vs_mode ? "yes" : "");
				}
				ImGui::TableSetColumnIndex(col++);
				// A locked room cannot be entered without the password at all: the server only
				// hands a password-less joiner a PUBLIC slot, and a locked room has none.
				ImGui::TextUnformatted(rooms[i].has_password ? "yes" : "");
				ImGui::TableSetColumnIndex(col);
				ImGui::Text("%llu", rooms[i].room_id);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		if (roomCount == 0)
		{
			ImGui::TextDisabled("No rooms found. Press Refresh, or host one yourself.");
		}

		// Join by ID stays available: a room can be joined before it shows up in a search, and it
		// is the fallback when someone simply gives you a number.
		ImGui::PushItemWidth(160.0f);
		ImGui::InputText("Room ID", m_netJoinRoomId, sizeof(m_netJoinRoomId), ImGuiInputTextFlags_CharsDecimal);
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGuiCustom::ButtonToggleable("Join room", m_netJoinRoomId[0] != '\0'))
		{
			net::JoinRoom(_strtoui64(m_netJoinRoomId, nullptr, 10), m_netRoomPassword);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("A locked room needs its password typed into the Room password box above.");
		}

		ImGui::NewLine();
		if (ImGui::Button("Disconnect"))
		{
			net::Disconnect();
		}
		break;
	}

	case YAMPNET_STATE_IN_ROOM:
	{
		if (status.room_id != 0)
		{
			ImGui::Text("Room ID: %llu", status.room_id);
			ImGui::SameLine();
			if (ImGui::Button("Copy"))
			{
				char idText[32];
				sprintf_s(idText, "%llu", status.room_id);
				ImGui::SetClipboardText(idText);
			}
		}
		// What this match will actually be played under, stated for BOTH players: the host has to
		// see what it published (the room is fixed now, so a later settings change is not it), and
		// the guest has to see what it has just adopted.
		const char* const bySetter = status.hosting ? "" : " (set by the host)";
		switch (CurrentRoomSetting())
		{
		case RoomSetting::Damage:
			ImGui::Text("Damage: %s%s", status.real_damage ? "Real" : "Normal", bySetter);
			break;
		case RoomSetting::Vf2Version:
			ImGui::Text("Version: %s%s", status.vf2_version20 ? "2.0" : "2.1", bySetter);
			break;
		case RoomSetting::Pre3Start:
			ImGui::Text("Rounds start: %s%s",
				status.pre3_vs_start ? "in a versus match" : "at power-on", bySetter);
			break;
		case RoomSetting::Src2Assign:
			// The room's GAME ASSIGNMENTS, one line - the same fields the browser lists, stated
			// for whoever is actually in the room. Absent for a room hosted by an older build.
			if (status.src2.present)
			{
				const auto name = [](const char* const* names, size_t count, uint8_t v)
				{
					return v < count ? names[v] : "?";
				};
				ImGui::Text("Assignments: %s, %s, motor %s, %s cabinet, ranking %s%s",
					name(SRC2_DIFFICULTY_NAMES, std::size(SRC2_DIFFICULTY_NAMES),
						status.src2.difficulty),
					name(SRC2_MODE_NAMES, std::size(SRC2_MODE_NAMES), status.src2.gameMode),
					name(SRC2_MOTOR_NAMES, std::size(SRC2_MOTOR_NAMES), status.src2.motorPower),
					name(SRC2_CABINET_NAMES, std::size(SRC2_CABINET_NAMES),
						status.src2.cabinetType),
					name(SRC2_RANKING_NAMES, std::size(SRC2_RANKING_NAMES), status.src2.ranking),
					bySetter);
			}
			break;
		case RoomSetting::None:
			break;
		}
		if (RoomHasVsMode())
		{
			// Stated for every m2ftg game, because they all read it and it changes the boot itself.
			ImGui::Text("Versus mode: %s%s", status.vs_mode ? "on" : "off", bySetter);
		}
		ImGui::PushTextWrapPos();
		if (status.hosting)
		{
			ImGui::TextUnformatted("Give this ID to the other player so they can join.");
		}
		// A LINKED-CABINET GAME HAS NO ROUND TO START. Virtual On's two cabinets find each other
		// through the ROM's own boot-time network check and stay linked from then on; there is no
		// barrier, nothing to reset and nothing for "Start match" to open. Telling the player to
		// press it would be telling them to press a button that does nothing - which is exactly
		// what the overlay used to do through an entire match.
		net::LinkedCabinetStatus lobbyLink = {};
		const bool linkedCabinet = GetLinkedCabinetStatus(lobbyLink);
		if (linkedCabinet)
		{
			ImGui::TextUnformatted("This game links its two cabinets itself. Once the other player "
				"is in the room the cabinets find each other, and you start a match by pressing "
				"START on each cabinet exactly as you would on the hardware.");
		}
		else
		{
			// The room is not a presence channel: this machine finds out the other player exists
			// only when their first packet arrives, and that does not happen until they start the
			// match. So "waiting for them to join" is not something the lobby can honestly display.
			ImGui::TextUnformatted("Both players press Start match. Nothing happens until both have - "
				"the board is reset on both machines at that point so the round starts from the same state.");
		}
		// FV2's round starts from a savestate; a linked cabinet has no round and reboots into its
		// ROLE instead. Saying the first to a Sega Racing Classic 2 player describes a mechanism
		// their session never uses.
		if (IsPre3Game() && !linkedCabinet)
		{
			ImGui::TextUnformatted("This board restores its own versus start state rather than "
				"rebooting, so the match picks up from there instead of from the attract screen. "
				"Your HLE hook settings are held at their defaults until the session ends.");
		}
		else if (IsPre3Game())
		{
			ImGui::TextUnformatted("Joining a room reboots this cabinet so it comes up as the "
				"MASTER or the SLAVE - the board reads that once at power-on, exactly as the real "
				"one does, so it cannot be changed on a running machine.");
		}
		ImGui::PopTextWrapPos();

		if (!linkedCabinet && ImGui::Button("Start match"))
		{
			net::RequestStartRound();
		}
		if (!linkedCabinet)
		{
			ImGui::SameLine();
		}
		if (ImGui::Button("Leave room"))
		{
			net::LeaveRoom();
		}
		break;
	}

	case YAMPNET_STATE_SYNCING:
	case YAMPNET_STATE_IN_MATCH:
	{
		ImGui::Text("You are player %d", status.local_player + 1);
		ImGui::Text("Input delay: %d frames", m_netFrameDelay);
		// Every stall is a frame the emulator could not run because the other machine's input had
		// not arrived. A number that keeps climbing is the connection, not the game.
		ImGui::Text("Stalls: %u", status.stall_count);
		if (ImGui::Button("Leave match"))
		{
			net::LeaveRoom();
		}
		break;
	}

	default:
		break;
	}
}

void YAMPUserInterface::DrawNetplayOverlay()
{
	if (!IsNetplayGame() || !net::IsAvailable())
	{
		return;
	}

	const net::Status status = net::GetStatus();

	// The other player vanished. This takes over the screen rather than joining the small status
	// overlay: the match is over, the game is about to be reset, and the player needs to know why
	// their opponent stopped moving instead of being dropped back into attract mode unexplained.
	if (status.peer_lost)
	{
		const ImVec2& display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos({ display.x / 2.0f, display.y / 2.0f }, ImGuiCond_Always,
			{ 0.5f, 0.5f });
		ImGui::Begin("Netplay##peerlost", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		ImGui::TextUnformatted(status.peer_lost_reason);
		ImGui::TextUnformatted("The session will be closed and the game reset.");
		ImGui::NewLine();
		if (ImGui::Button("OK", { 120.0f, 0.0f }))
		{
			// Leave RPCN entirely (not just the room) so nothing reconnects behind the player's
			// back, then put the board back to a clean attract mode.
			net::EndSession();
			NetplayResetBoard();
		}
		ImGui::End();
		return;
	}

	// A LINKED-CABINET GAME REPORTS ITS LINK, NOT A ROUND - but it is still an ordinary session,
	// so it keeps the ordinary overlay and only the round-specific wording is replaced.
	//
	// Virtual On has no barrier and no round: the ROM's own boot-time link check is what starts
	// play, so the session sits at IN_ROOM for the whole match and never reaches IN_MATCH - the
	// state this overlay uses to decide it should get out of the way. Left alone, that put a
	// "Both players press Start match" splash over an entire linked match, telling both players to
	// press a button that does nothing for this game.
	//
	// NOTE the ordering. `GetLinkedCabinet` answers yes as soon as a cabinet role is applied and
	// the board has booted, which says nothing about whether a SESSION exists - so it must not be
	// consulted before the checks below. Doing that swallowed the room id (still 0 while
	// connecting), the error text and the board-booting line, and drew a permanent box in offline
	// solo play for anyone whose cabinet role was not NOLINK.
	net::LinkedCabinetStatus link = {};
	const bool linkedCabinet = GetLinkedCabinetStatus(link);

	// Nothing to say before a session is started, and nothing worth covering the game with once
	// one is running normally.
	if (!status.started || status.state == YAMPNET_STATE_IN_MATCH || status.state == YAMPNET_STATE_IDLE)
	{
		return;
	}

	// The linked-cabinet equivalent of reaching IN_MATCH: the ring is up and the ROM's own network
	// check has passed, so the cabinets are talking and there is nothing left to tell anyone.
	if (linkedCabinet && link.ringUp && link.checkDone)
	{
		return;
	}

	ImGui::SetNextWindowPos({ 20, 20 }, ImGuiCond_Always);
	ImGui::Begin("Netplay", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
	// `status.text` for IN_ROOM asks both players to press Start match, which is the one thing a
	// linked-cabinet game must not say. Every other state's text is accurate for it.
	if (linkedCabinet && status.state == YAMPNET_STATE_IN_ROOM)
	{
		ImGui::Text("%s cabinet.", link.role == 1 ? "MASTER" : "SLAVE");
		ImGui::TextUnformatted(link.ringUp
			? "Ring up - the cabinet is running its network check."
			: "Waiting for the other cabinet to join...");
		ImGui::TextDisabled("The cabinets link themselves. Press START on each to play.");
	}
	else
	{
		ImGui::TextUnformatted(status.text);
	}
	// Shown for as long as there is a room, not just while IN_ROOM: a command-line host opens the
	// barrier the same frame it gets the room, so it is SYNCING within a frame or two - and that
	// is exactly when the other player still needs to be told which room to join.
	if (status.room_id != 0)
	{
		ImGui::Text("Room ID: %llu", status.room_id);
	}
	// The round cannot start until this machine's board has booted (see m2ftg::IsBoardBooted /
	// pre3::IsBoardBooted). Say so, or the wait looks like the session having stalled.
	if ((status.state == YAMPNET_STATE_IN_ROOM || status.state == YAMPNET_STATE_SYNCING)
		&& !NetplayBoardBooted())
	{
		ImGui::TextUnformatted("Waiting for the emulated board to finish booting...");
	}
	if (status.state == YAMPNET_STATE_FAILED && status.error != nullptr && *status.error != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", status.error);
	}
	if (status.desynced)
	{
		ImGui::TextColored(WARNING_COLOUR, "Desync at frame %u - round ended", status.desync_frame);
	}
	ImGui::TextDisabled("F1 -> Netplay");
	ImGui::End();
}
