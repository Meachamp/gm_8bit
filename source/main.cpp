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
#include <checksum_crc.h>
#include <audio_effects.h>
#include <net.h>
#include <minmax.h>
#include <thirdparty.h>
#include <steam_voice.h>

#define VOICE_DATA_SZ 0xE
#define OFFSET_TO_VOICE_SZ 0xC
#define OFFSET_TO_CODEC_OP 0xB
#define CODEC_OP_OPUSPLC 6
#define MIN_PCKT_SZ VOICE_DATA_SZ + sizeof(CRC32_t)

#ifdef SYSTEM_WINDOWS
	#include <windows.h>
	static const uint8_t GMOD_SV_BroadcastVoice_sym_sig[] = "\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****";
	static const size_t GMOD_SV_BroadcastVoice_siglen = sizeof(GMOD_SV_BroadcastVoice_sym_sig) - 1;

	static const uint8_t CreateOpusPLCCodec_sig[] = "\x56\x6A\x48\xE8\x98\x1B\x49\x00\x8B\xF0\x83\xC4\x04\x33\xC0\x85";
	static const size_t CreateOpusPLCCodec_siglen = sizeof(CreateOpusPLCCodec_sig) - 1;
#endif

#ifdef SYSTEM_LINUX
	#include <dlfcn.h>
	static const char* GMOD_SV_BroadcastVoice_sym_sig = "_Z21SV_BroadcastVoiceDataP7IClientiPcx";
	static const uint8_t CreateOpusPLCCodec_sig[] = "\x57\x56\x53\xE8\x03\xDC\xD0\xFF\x81\xC3\x54\xE9\x40\x01\x83\xEC";
	static const size_t CreateOpusPLCCodec_siglen = sizeof(CreateOpusPLCCodec_sig) - 1;
#endif

static int crushFactor = 350;
static float gainFactor = 1.2;
static bool broadcastPackets = false;

static char decompressedBuffer[20 * 1024];
static char recompressBuffer[20 * 1024];

typedef IVoiceCodec* (*CreateOpusPLCCodecProto)();
CreateOpusPLCCodecProto func_CreateOpusPLCCodec;

SourceSDK::ModuleLoader* steamclient_loader = nullptr;
SourceSDK::ModuleLoader* engine_loader = nullptr;
Net* net_handl = nullptr;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
Detouring::Hook detour_BroadcastVoiceData;

std::unordered_map<int, IVoiceCodec*> afflicted_players;

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

	if (broadcastPackets && nBytes > sizeof(uint64_t)) {
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
		IVoiceCodec* codec = afflicted_players.at(uid);

		#ifdef _DEBUG
			std::cout << "Received packet of length: " << nBytes << std::endl;
			std::cout << "OP0: " << (int)data[0x8] << " " << *(short*)&data[0x9] << std::endl;
		#endif

		if(nBytes < MIN_PCKT_SZ || data[OFFSET_TO_CODEC_OP] != CODEC_OP_OPUSPLC) {
			#ifdef _DEBUG
				if(nBytes >= MIN_PCKT_SZ) {
					std::cout << "Ignoring voice packet with OPCODE: " << (int)data[OFFSET_TO_CODEC_OP] << " OP0: " << (int)data[0x8] << std::endl;
				} else {
					std::cout << "Ignoring voice packet with size: " << nBytes << std::endl;
				}
			#endif

			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int samples = codec->Decompress(data + VOICE_DATA_SZ, nBytes - VOICE_DATA_SZ - sizeof(CRC32_t), (char*)decompressedBuffer, sizeof(decompressedBuffer));
		if (samples <= 0) {
			//Just hit the trampoline at this point.
			std::cout << "Decompression failed: " << samples << std::endl;
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Decompressed samples " << samples << std::endl;
		#endif

		//Bit crush the stream
		AudioEffects::BitCrush((uint16_t*)&decompressedBuffer, samples, crushFactor, gainFactor);

		//Recompress the stream
		int bytesWritten = codec->Compress((char*)decompressedBuffer, samples, recompressBuffer + VOICE_DATA_SZ, sizeof(recompressBuffer) - VOICE_DATA_SZ - sizeof(CRC32_t), false);

		//Fixup original packet
		memcpy(recompressBuffer, data, VOICE_DATA_SZ);
		uint16_t* dataLen = (uint16_t*)(recompressBuffer + OFFSET_TO_VOICE_SZ);
		*dataLen = bytesWritten;

		//Fixup checksum
		CRC32_t crc = CRC32_ProcessSingleBuffer(recompressBuffer, VOICE_DATA_SZ + bytesWritten);
		*(CRC32_t*)(recompressBuffer + VOICE_DATA_SZ + bytesWritten) = crc;

		uint32_t total_sz = bytesWritten + VOICE_DATA_SZ + sizeof(CRC32_t);

		#ifdef _DEBUG
			std::cout << "Retransmitted pckt size: " << total_sz << std::endl;
		#endif

		//Broadcast voice data with our updated compressed data.
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, total_sz, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

LUA_FUNCTION_STATIC(eightbit_crush) {
	crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_gain) {
	gainFactor = (float)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_broadcast) {
	broadcastPackets = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_getcrush) {
	LUA->PushNumber(crushFactor);
	return 1;
}

LUA_FUNCTION_STATIC(eightbit_enable8bit) {
	int id = LUA->GetNumber(1);
	bool b = LUA->GetBool(2);

	if (afflicted_players.find(id) != afflicted_players.end() && b) {
		return 0;
	}

	if (b) {
		IVoiceCodec* codec = func_CreateOpusPLCCodec();
		codec->Init(5, 24000);
		afflicted_players.insert(std::pair<int, IVoiceCodec*>(id, codec));
	}
	else if(afflicted_players.find(id) != afflicted_players.end()) {
		IVoiceCodec* codec = afflicted_players.at(id);
		codec->Release();
		afflicted_players.erase(id);
	}

	return 0;
}


GMOD_MODULE_OPEN()
{
	afflicted_players = std::unordered_map<int, IVoiceCodec*>();

	engine_loader = new SourceSDK::ModuleLoader("engine");
	SymbolFinder symfinder;

	#ifdef SYSTEM_WINDOWS
		void* sv_bcast = symfinder.FindPattern(engine_loader->GetModule(), GMOD_SV_BroadcastVoice_sym_sig, GMOD_SV_BroadcastVoice_siglen);
	#elif SYSTEM_LINUX
		void* sv_bcast = symfinder.FindSymbol(engine_loader->GetModule(), GMOD_SV_BroadcastVoice_sym_sig);
	#endif
	if (sv_bcast == nullptr) {
		LUA->ThrowError("Could not locate SV_BrodcastVoice symbol!");
	}

	#ifdef SYSTEM_LINUX
		steamclient_loader = new SourceSDK::ModuleLoader("steamclient");
		if(steamclient_loader->GetModule() == nullptr) {
			LUA->ThrowError("Could not load steamclient!");
		}
		void* codecPtr = symfinder.FindPattern(steamclient_loader->GetModule(), CreateOpusPLCCodec_sig, CreateOpusPLCCodec_siglen);
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

		LUA->PushString("Enable8Bit");
		LUA->PushCFunction(eightbit_enable8bit);
		LUA->SetTable(-3);

		LUA->PushString("EnableBroadcast");
		LUA->PushCFunction(eightbit_broadcast);
		LUA->SetTable(-3);

		LUA->PushString("SetGainFactor");
		LUA->PushCFunction(eightbit_gain);
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

	for (auto& p : afflicted_players) {
		if (p.second != nullptr) {
			p.second->Release();
		}
	}

	afflicted_players.clear();

	delete steamclient_loader;
	delete engine_loader;
	delete net_handl;

	return 0;
}