/******************************************************************************
* Copyright (c) 2019, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/builder/clipper.hpp>

#include <entwine/builder/chunk.hpp>
#include <entwine/builder/chunk-cache.hpp>

namespace entwine
{

Clipper::~Clipper()
{
    for (
            uint64_t depth(0);
            depth < m_slow.size() && m_slow[depth].size();
            ++depth)
    {
        // Purging everything, insert everything into our aged list so we
        // expire everything.
        UsedMap& used(m_slow[depth]);
        UsedMap& aged(m_aged[depth]);
        aged.insert(used.begin(), used.end());
    }

    clip();
}

Chunk* Clipper::get(const ChunkKey& ck)
{
    CachedChunk& fast(m_fast[ck.depth()]);
    if (fast.xyz == ck.position()) return fast.chunk;

    auto& slow(m_slow[ck.depth()]);
    auto it = slow.find(ck.position());

    if (it == slow.end())
    {
        auto& aged(m_aged[ck.depth()]);
        auto agedIt = aged.find(ck.position());
        if (agedIt == aged.end()) return nullptr;

        it = slow.insert(std::make_pair(agedIt->first, agedIt->second)).first;
        aged.erase(agedIt);
    }

    fast.xyz = ck.position();
    return fast.chunk = it->second;
}

void Clipper::set(const ChunkKey& ck, Chunk* chunk)
{
    CachedChunk& fast(m_fast[ck.depth()]);

    fast.xyz = ck.position();
    fast.chunk = chunk;

    auto& slow(m_slow[ck.depth()]);
    assert(!slow.count(ck.position()));
    slow[ck.position()] = chunk;
}

void Clipper::clip()
{
    m_fast.fill(CachedChunk());

    for (
            uint64_t depth(0);
            depth < m_slow.size() && (
                m_slow[depth].size() || m_aged[depth].size()
            );
            ++depth)
    {
        if (m_slow[depth].empty() && m_aged[depth].empty()) continue;

        UsedMap& used(m_slow[depth]);
        UsedMap& aged(m_aged[depth]);

        // Whatever is in our aging list hasn't been touched in two iterations,
        // so deref those chunks.
        m_cache.clip(depth, aged);

        // Get rid of what we just clipped, and lower our recently touched
        // list into our aging list.
        aged.clear();

        // Now clear our "used" list, moving it into "aged".
        std::swap(used, aged);
    }

    m_cache.clipped();
}

} // namespace entwine

