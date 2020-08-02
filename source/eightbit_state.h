#pragma once

struct EightbitState {
	int crushFactor = 350;
	float gainFactor = 1.2;
	bool broadcastPackets = false;
	int desampleRate = 2;
	std::unordered_map<int, std::tuple<IVoiceCodec*, int>> afflictedPlayers;
};