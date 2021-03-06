#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iostream>
#include <cbase.h>
#include <eifacev21.h>
#include <ivoicecodec.h>
#include <iclient.h>
#include <unordered_map>
#include <audio_effects.h>
#include <net.h>
#include <minmax.h>
#include <thirdparty.h>
#include <steam_voice.h>
#include <eightbit_state.h>
#include <GarrysMod/Symbol.hpp>

#define STEAM_PCKT_SZ sizeof(uint64_t) + sizeof(CRC32_t)

#ifdef SYSTEM_WINDOWS
	#include <windows.h>

	const std::vector<Symbol> BroadcastVoiceSyms = {
		Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50")
	};

	static const uint8_t CreateOpusPLCCodec_sig[] = "\x56\x6A\x48\xE8****\x8B\xF0\x83\xC4\x04\x33\xC0\x85\xF6**\x50\x50\x50\x8D\x4E\x18******\xC6\x46\x04\x01";
	static const size_t CreateOpusPLCCodec_siglen = sizeof(CreateOpusPLCCodec_sig) - 1;
#endif

#ifdef SYSTEM_LINUX
	#include <dlfcn.h>
	const std::vector<Symbol> BroadcastVoiceSyms = {
		Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx")
	};

	static const uint8_t CreateOpusPLCCodec_sig[] = "\x57\x56\x53\xE8****\x81\xC3****\x83\xEC\x10\xC7\x04\x24\x50\x00\x00\x00\xE8****\x31\xD2\x89\xC6\x8D\x83****\xC6\x46\x04\x01";
	static const size_t CreateOpusPLCCodec_siglen = sizeof(CreateOpusPLCCodec_sig) - 1;
#endif

static char decompressedBuffer[20 * 1024];
static char recompressBuffer[20 * 1024];

typedef IVoiceCodec* (*CreateOpusPLCCodecProto)();
CreateOpusPLCCodecProto func_CreateOpusPLCCodec;

Net* net_handl = nullptr;
EightbitState* g_eightbit = nullptr;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
Detouring::Hook detour_BroadcastVoiceData;

void hook_BroadcastVoiceData(IClient* cl, uint nBytes, char* data, int64 xuid) {
	//Check if the player is in the set of enabled players.
	//This is (and needs to be) and O(1) operation for how often this function is called. 
	//If not in the set, just hit the trampoline to ensure default behavior. 
	int uid = cl->GetUserID();

#ifdef THIRDPARTY_LINK
	if(checkIfMuted(cl->GetPlayerSlot()+1)) {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
#endif

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (g_eightbit->broadcastPackets && nBytes > sizeof(uint64_t)) {
		//Get the user's steamid64, put it at the beginning of the buffer. 
		//Notice that we don't use the conveniently provided one in the voice packet. The client can manipulate that one.
		uint64_t id64 = *(uint64_t*)((char*)cl + 181);
		*(uint64_t*)decompressedBuffer = id64;

		//Transfer the packet data to our scratch buffer
		//This looks jank, but it's to prevent a theoretically malformed packet triggering a massive memcpy
		size_t toCopy = nBytes - sizeof(uint64_t);
		std::memcpy(decompressedBuffer + sizeof(uint64_t), data + sizeof(uint64_t), toCopy);

		//Finally we'll broadcast our new packet
		net_handl->SendPacket("127.0.0.1", decompressedBuffer, nBytes);
	}

	if (afflicted_players.find(uid) != afflicted_players.end()) {
		IVoiceCodec* codec = std::get<0>(afflicted_players.at(uid));

		if(nBytes < STEAM_PCKT_SZ) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int bytesDecompressed = SteamVoice::DecompressIntoBuffer(codec, data, nBytes, decompressedBuffer, sizeof(decompressedBuffer));
		int samples = bytesDecompressed / 2;
		if (bytesDecompressed <= 0) {
			//Just hit the trampoline at this point.
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Decompressed samples " << samples << std::endl;
		#endif

		//Apply audio effect
		int eff = std::get<1>(afflicted_players.at(uid));
		switch (eff) {
		case AudioEffects::EFF_BITCRUSH:
			AudioEffects::BitCrush((uint16_t*)&decompressedBuffer, samples, g_eightbit->crushFactor, g_eightbit->gainFactor);
			break;
		case AudioEffects::EFF_DESAMPLE:
			AudioEffects::Desample((uint16_t*)&decompressedBuffer, samples, g_eightbit->desampleRate);
			break;
		default:
			break;
		}		

		//Recompress the stream
		uint64_t steamid = *(uint64_t*)data;
		int bytesWritten = SteamVoice::CompressIntoBuffer(steamid, codec, decompressedBuffer, samples*2, recompressBuffer, sizeof(recompressBuffer), 24000);
		if (bytesWritten <= 0) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Retransmitted pckt size: " << bytesWritten << std::endl;
		#endif

		//Broadcast voice data with our updated compressed data.
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

LUA_FUNCTION_STATIC(eightbit_crush) {
	g_eightbit->crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_gain) {
	g_eightbit->gainFactor = (float)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_broadcast) {
	g_eightbit->broadcastPackets = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_getcrush) {
	LUA->PushNumber(g_eightbit->crushFactor);
	return 1;
}

LUA_FUNCTION_STATIC(eightbit_setdesamplerate) {
	g_eightbit->desampleRate = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_enableEffect) {
	int id = LUA->GetNumber(1);
	int eff = LUA->GetNumber(2);

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (afflicted_players.find(id) != afflicted_players.end()) {
		if (eff == AudioEffects::EFF_NONE) {
			IVoiceCodec* codec = std::get<0>(afflicted_players.at(id));
			codec->Release();
			afflicted_players.erase(id);
		}
		else {
			std::get<1>(afflicted_players.at(id)) = eff;
		}
		return 0;
	}
	else if(eff != AudioEffects::EFF_NONE) {
		IVoiceCodec* codec = func_CreateOpusPLCCodec();
		codec->Init(5, 24000);
		afflicted_players.insert(std::pair<int, std::tuple<IVoiceCodec*, int>>(id, std::tuple<IVoiceCodec*, int>(codec, eff)));
	}
	return 0;
}


GMOD_MODULE_OPEN()
{
	g_eightbit = new EightbitState();

	SourceSDK::ModuleLoader engine_loader("engine");
	SymbolFinder symfinder;

	void* sv_bcast = nullptr;

	for (const auto& sym : BroadcastVoiceSyms) {
		sv_bcast = symfinder.Resolve(engine_loader.GetModule(), sym.name.c_str(), sym.length);

		if (sv_bcast)
			break;
	}

	if (sv_bcast == nullptr) {
		LUA->ThrowError("Could not locate SV_BroadcastVoice symbol!");
	}

	#ifdef SYSTEM_LINUX
		SourceSDK::ModuleLoader steamclient_loader("steamclient");
		if(steamclient_loader.GetModule() == nullptr) {
			LUA->ThrowError("Could not load steamclient!");
		}
		void* codecPtr = symfinder.FindPattern(steamclient_loader.GetModule(), CreateOpusPLCCodec_sig, CreateOpusPLCCodec_siglen);
	#elif SYSTEM_WINDOWS
		//Windows loads steamclient from a directory outside of the normal search paths. 
		//This is our workaround.
		void* steamlib = LoadLibraryA("steamclient.dll");
		if (steamlib == nullptr) {
			LUA->ThrowError("[WIN] Could not load steamclient!");
		}
		void* codecPtr = symfinder.FindPattern(steamlib, CreateOpusPLCCodec_sig, CreateOpusPLCCodec_siglen);
	#endif
	
	if (codecPtr == nullptr) {
		LUA->ThrowError("Could not locate CreateOpusPLCCodec!");
	}

	func_CreateOpusPLCCodec = (CreateOpusPLCCodecProto)codecPtr;

	detour_BroadcastVoiceData.Create(Detouring::Hook::Target(sv_bcast), reinterpret_cast<void*>(&hook_BroadcastVoiceData));
	detour_BroadcastVoiceData.Enable();

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("eightbit");
	LUA->CreateTable();
		LUA->PushString("SetCrushFactor");
		LUA->PushCFunction(eightbit_crush);
		LUA->SetTable(-3);

		LUA->PushString("GetCrushFactor");
		LUA->PushCFunction(eightbit_getcrush);
		LUA->SetTable(-3);

		LUA->PushString("EnableEffect");
		LUA->PushCFunction(eightbit_enableEffect);
		LUA->SetTable(-3);

		LUA->PushString("EnableBroadcast");
		LUA->PushCFunction(eightbit_broadcast);
		LUA->SetTable(-3);

		LUA->PushString("SetGainFactor");
		LUA->PushCFunction(eightbit_gain);
		LUA->SetTable(-3);

		LUA->PushString("SetDesampleRate");
		LUA->PushCFunction(eightbit_setdesamplerate);
		LUA->SetTable(-3);

		LUA->PushString("EFF_NONE");
		LUA->PushNumber(AudioEffects::EFF_NONE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_DESAMPLE");
		LUA->PushNumber(AudioEffects::EFF_DESAMPLE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_BITCRUSH");
		LUA->PushNumber(AudioEffects::EFF_BITCRUSH);
		LUA->SetTable(-3);
	LUA->SetTable(-3);
	LUA->Pop();

	net_handl = new Net();

#ifdef THIRDPARTY_LINK
	linkMutedFunc();
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
	detour_BroadcastVoiceData.Destroy();

	for (auto& p : g_eightbit->afflictedPlayers) {
		IVoiceCodec* codec = std::get<0>(p.second);
		if (codec != nullptr) {
			codec->Release();
		}
	}

	delete net_handl;
	delete g_eightbit;

	return 0;
}