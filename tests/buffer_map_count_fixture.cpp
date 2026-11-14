#include "buffer_map_count.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

int main() {
    BufferMapCount state;
    assert(!state.any() && !state.remove());
    assert(state.add() && state.add() && state.any());
    assert(state.remove() && state.any() && state.remove() && !state.any());
    state.count=UINT32_MAX;
    assert(!state.add() && state.count==UINT32_MAX);
    assert(state.remove() && state.add() && !state.add() && state.count==UINT32_MAX);
    state.count=0;
    std::vector<std::thread> workers;
    for(unsigned t=0;t<8;++t)workers.emplace_back([&]{for(unsigned i=0;i<50000;++i)assert(state.add());});
    for(auto& worker:workers)worker.join();
    assert(state.count==400000);workers.clear();
    for(unsigned t=0;t<8;++t)workers.emplace_back([&]{for(unsigned i=0;i<50000;++i)assert(state.remove());});
    for(auto& worker:workers)worker.join();
    assert(!state.any() && !state.remove());
    std::unordered_map<unsigned,BufferMapCount> identities;
    auto* alias=&identities.try_emplace(0).first->second;
    assert(alias->add());
    for(unsigned i=1;i<4096;++i)identities.try_emplace(i);
    assert(alias==&identities.at(0) && alias->remove() && !identities.at(0).any());
    std::puts("{\"success\":true,\"concurrent_operations\":800000,\"saturation\":true,\"rehash\":true}");
}
