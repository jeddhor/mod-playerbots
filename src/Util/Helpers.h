/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_HELPERS_H
#define PLAYERBOTS_HELPERS_H

#include "Random.h"
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

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

/**
 * Case-insensitive substring search.
 *
 * @param haystack The string to search in
 * @param needle   The substring to search for
 * @return Pointer to the first matching position in haystack, or nullptr if not found.
 */
char* strstri(char const* haystack, char const* needle);

/**
 * Trim whitespace from the left side of a string (in place).
 *
 * @param s The string to trim
 * @return Reference to the modified string
 */
std::string& ltrim(std::string& s);

/**
 * Trim whitespace from the right side of a string (in place).
 *
 * @param s The string to trim
 * @return Reference to the modified string
 */
std::string& rtrim(std::string& s);

/**
 * Trim whitespace from both ends of a string (in place).
 *
 * @param s The string to trim
 * @return Reference to the modified string
 */
std::string& trim(std::string& s);

/**
 * Split a string using a C-string delimiter.
 *
 * @param dest  Vector to store split tokens
 * @param str   String to split
 * @param delim C-string delimiter
 */
void split(std::vector<std::string>& dest, std::string const str, char const* delim);

/**
 * Split a string using a single character delimiter.
 *
 * @param s     String to split
 * @param delim Delimiter character
 * @param elems Vector to store split tokens
 * @return Reference to the vector containing tokens
 */
std::vector<std::string>& split(std::string const s, char delim, std::vector<std::string>& elems);

/**
 * Split a string using a single character delimiter.
 *
 * @param s     String to split
 * @param delim Delimiter character
 * @return Vector containing split tokens
 */
std::vector<std::string> split(std::string const s, char delim);

#endif
