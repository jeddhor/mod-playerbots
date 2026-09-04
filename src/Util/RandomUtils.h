/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RANDOMUTILS_H
#define PLAYERBOTS_RANDOMUTILS_H

#include "Random.h"
#include <cstddef>
#include <iterator>

// Deliberately a header of its own rather than part of Helpers.h: these are pulled in by widely
// included headers such as TravelNode.h, and Helpers.h also declares ltrim/rtrim/trim/split, which
// collide with the file-local helpers some translation units define for themselves.

/**
 * Pick a uniformly random element from a container, or nullptr if it is empty.
 *
 * `urand(0, container.size() - 1)` is used in dozens of places in this module. When the container
 * is empty, `size() - 1` wraps to SIZE_MAX, truncates to 0xFFFFFFFF, and urand returns an arbitrary
 * index - an out-of-bounds read. Every one of those sites relies on a separate emptiness check
 * somewhere upstream. Prefer these helpers, which make the empty case explicit and unmissable.
 *
 * @param container Any container with size() and random-access begin()
 * @return Pointer to a random element, or nullptr if the container is empty
 */
template <class C>
auto RandomElement(C& container) -> decltype(&*std::begin(container))
{
    if (container.empty())
        return nullptr;

    return &*(std::begin(container) + urand(0, static_cast<uint32>(container.size()) - 1));
}

/**
 * Pick a uniformly random valid index into a container.
 *
 * @param container Any container with size()
 * @param index     Set to the chosen index when the container is non-empty
 * @return false if the container is empty, in which case `index` is untouched
 */
template <class C>
bool RandomIndex(C const& container, size_t& index)
{
    if (container.empty())
        return false;

    index = urand(0, static_cast<uint32>(container.size()) - 1);
    return true;
}

#endif
