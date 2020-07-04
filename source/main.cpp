#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iclient_patch.h>
#include <iostream>
#include <cbase.h>
#include <eifacev21.h>
#include <ivoicecodec.h>
#include <unordered_map>


#include <dlfcn.h>


static const char* GMOD_SV_BroadcastVoice_sym_sig = "_Z21SV_BroadcastVoiceDataP7IClientiPcx";
static const uint8_t CreateSilkCodec_sig[] = "\x57\x56\x53\xE8\xA3\xDC\xD0\xFF\x81\xC3\xF4\xE9\x40\x01\x83\xEC";
//static const uint8_t CreateSilkCodec_sig[] = "\x57\x56\x53\xE8****\x81\xC3****\x83\xEC\x10\xC7\x04\x24\x78\x00\x00\x00\xE8****\x89\xC6";
static const size_t CreateSilkCodec_siglen = sizeof(CreateSilkCodec_sig) - 1;

static int crushFactor = 700;
static short decompressedBuffer[11500*2];
static char recompressBuffer[11500*4];
static bool didInit = false;

typedef IVoiceCodec* (*CreateSilkCodecProto)();
CreateSilkCodecProto func_CreateSilkCodec;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
Detouring::Hook detour_BroadcastVoiceData;

std::unordered_map<int, IVoiceCodec*> afflicted_players;

void hook_BroadcastVoiceData(IClient* cl, int nBytes, char* data, int64 xuid) {
	//Check if the player is in the set of enabled players.
	//This is (and needs to be) and O(1) operation for how often this function is called. 
	//If not in the set, just hit the trampoline to ensure default behavior. 
	int uid = cl->GetUserID();
	if (afflicted_players.find(uid) != afflicted_players.end()) {
		//Decompress the stream with vaudio_speex
		//Produces signed 16-bit PCM samples @ 11025 Hz
		//Output is # samples produced.
		IVoiceCodec* codec = afflicted_players.at(uid);
		std::cout << "Decomp" << nBytes << std::endl;

		if(nBytes < 12) return;

		int samples = codec->Decompress(data + 8, nBytes - 8, (char*)decompressedBuffer, sizeof(decompressedBuffer));
		//Speex will return a negative (or zeroed) number if decompression fails. 
		if (samples <= 0) {
			//Just hit the trampoline at this point.
			std::cout << "Decompression failed: " << samples << std::endl;
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		//Bit crush the stream
		for (int i = 0; i < samples; i++) {
			short* ptr = &decompressedBuffer[i];

			//Signed shorts range from -32768 to 32767
			//Let's quantize that a bit
			float f = (float)*ptr;
			f /= crushFactor;
			*ptr = (short)f;
			*ptr *= crushFactor;
		}

		//Recompress the stream
		int bytesWritten = codec->Compress((char*)decompressedBuffer, samples, recompressBuffer, sizeof(recompressBuffer), true);

		//Broadcast voice data with our update compressed data.
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

void* LoadSteamclient() {
	void* module = dlopen("bin/steamclient.so", RTLD_NOW);
	return module;
}

LUA_FUNCTION_STATIC(zsutil_crush) {
	crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(zsutil_enable8bit) {
	int id = LUA->GetNumber(1);
	bool b = LUA->GetBool(2);

	if (!didInit) {
		LUA->ThrowError("Module did not successfully init!");
		return 0;
	}

	if (afflicted_players.find(id) != afflicted_players.end() && b) {
		return 0;
	}

	if (b) {
		IVoiceCodec* codec = func_CreateSilkCodec();
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
	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("zsutil_crush");
	LUA->PushCFunction(zsutil_crush);
	LUA->SetTable(-3);

	LUA->PushString("Enable8Bit");
	LUA->PushCFunction(zsutil_enable8bit);
	LUA->SetTable(-3);

	SourceSDK::FactoryLoader engine_loader("engine");
	SymbolFinder symfinder;

	#ifdef SYSTEM_WINDOWS
		void* sv_bcast = symfinder.FindPattern(engine_loader.GetModule(), GMOD_SV_BroadcastVoice_sym_sig, GMOD_SV_BroadcastVoice_siglen, engine_loader.GetModule());
	#elif SYSTEM_LINUX
		void* sv_bcast = symfinder.FindSymbol(engine_loader.GetModule(), GMOD_SV_BroadcastVoice_sym_sig);
	#endif
	if (sv_bcast == nullptr) {
		std::cout << "Could not locate SV_BrodcastVoice symbol!" << std::endl;
		return 0;
	}

	LoadSteamclient();

	SourceSDK::FactoryLoader steamclient_loader("steamclient");
	std::cout << steamclient_loader.GetModule() << std::endl;
	void *codecPtr = symfinder.FindPattern(steamclient_loader.GetModule(), CreateSilkCodec_sig, CreateSilkCodec_siglen);
	
	if (codecPtr == nullptr) {
		std::cout << "Could not locate CreateSilkCodec!" << std::endl;
		return 0;
	}

	std::cout << codecPtr << std::endl;

	func_CreateSilkCodec = (CreateSilkCodecProto)codecPtr;

	detour_BroadcastVoiceData.Create(Detouring::Hook::Target(sv_bcast), reinterpret_cast<void*>(&hook_BroadcastVoiceData));
	detour_BroadcastVoiceData.Enable();

	afflicted_players = std::unordered_map<int, IVoiceCodec*>();
	didInit = true;
	return 0;
}

GMOD_MODULE_CLOSE()
{
	detour_BroadcastVoiceData.Destroy();
	didInit = false;
	func_CreateSilkCodec = nullptr;

	for (auto& p : afflicted_players) {
		if (p.second != nullptr) {
			p.second->Release();
		}
	}

	afflicted_players.clear();

	return 0;
}