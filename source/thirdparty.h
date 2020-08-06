#pragma once
#ifdef __linux
#include <dlfcn.h>
#define THIRDPARTY_LINK

typedef bool (*MuteFuncProto)(int entidx);
MuteFuncProto external_mute_func = nullptr;

bool checkIfMuted(int entidx) {
    if(external_mute_func) {
        return external_mute_func(entidx);
    }
    return false;
}

void linkMutedFunc() {
    void* ptr = dlopen("gmsv_zsvoicechat_linux.dll", RTLD_NOW | RTLD_NOLOAD);
    if(ptr) {
        external_mute_func = (MuteFuncProto)dlsym(ptr, "shouldBeMuted");
        dlclose(ptr);
    }
}

#endif