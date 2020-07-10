# gm_8bit
A module for manipulating voice data in Garry's Mod. Currently it is in the early stages of prototyping, and many things may be subject to change. 

# What does it do?
gm_8bit is designed to a starting point for any kind of voice stream manipulation you might want to do on a Garry's Mod server (or any source engine server, with a bit of adjustment). 

gm_8bit can decompress and recompress steam voice packets. It includes an SV_BroadcastVoiceData hook to allow server operators to incercept and manipulate this voice data. It makes several things possible, include:
* Relaying server voice data to external locations
* Performing voice recognition and producing transcripts
* Recording voice data in compressed or uncompressed form
* Applying transformation to user voice streams, for example pitch correction, noise suppression, or gain control. 

gm_8bit currently has reference implementations for relaying voice data and applying transformations to voice streams. See the `voice-relay` repository for an example implementation of a server that uses gm_8bit to relay server voice communications to a discord channel. 

# Builds
Both windows and linux builds are available with every commit. See the actions page. 

# API
`eightbit.EnableBroadcast(bool)` Sets whether the module should relay voice packets to `localhost:4000`
`eightbit.Enable8Bit(userid, bool)` Sets whether to enable bit crushing for a given userid
`eightbit.SetGainFactor(number)` Sets the gain multiplier to apply to affected userids
`eightbit.SetCrushFactor(number)` Sets the bitcrush factor for the reference bitcrush implementation

