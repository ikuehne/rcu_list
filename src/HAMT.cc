#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "HAMT.hh"

// We do some sketchy memory stuff that GCC doesn't like. Disable that
// warning.
#ifdef __GNUC__
#ifndef __clang__
    #pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
#endif

#define LIKELY(condition) __builtin_expect(static_cast<bool>(condition), 1)
#define UNLIKELY(condition) __builtin_expect(static_cast<bool>(condition), 0)

//////////////////////////////////////////////////////////////////////////////
// Hash definitions.
//

// Get a "backup hash" to resolve collisions.
//
// The parameter gives how many backup hashes have already been used
// (starting at 0 for a single hash collision).
//
// Collisions of the original hash are extremely unlikely, so once we get
// to this point average efficiency doesn't matter. Worst-case asymptotic
// performance, however, *does* matter. We use the following procedure to
// get keys that will guarantee a separation in a number of steps linear
// in the size of the key (what hash tables conventionally call "constant
// time"):
//     - At each level of hashing, we take four more bytes off the string.
//     - Each byte from the string maps to two bytes in the "hash"; the first
//       is from the string, and the second is 1 if we've passed the end of the
//       string and 0 otherwise.
// Since we use up 4 bytes per iteration of this procedure, we'll separate
// the key from any different in time and space linear in the size of the
// key. 
uint64_t getNthBackup(const std::string &str, unsigned n)  {
    std::uint64_t result = 0;
    uint8_t *bytes = (uint8_t *)&result;

    for (size_t i = 0; i < 4; i++) {
        size_t idx = i + 4 * n;
        if (idx < str.size()) {
            bytes[2 * i] = str[idx];
        } else {
            bytes[2 * i + 1] = 1;
        }
    }

    return result;
}

//////////////////////////////////////////////////////////////////////////////
// TopLevelHamtNode method definitions.
//

void TopLevelHamtNode::insert(uint64_t hash, std::string &&str) {
    HamtNodeEntry *entryToInsert = &table[hash & FIRST_N_BITS];
    unsigned level = 0;

    if (entryToInsert->isNull()) {
        auto leaf = std::make_unique<HamtLeaf>(std::move(str), hash);
        *entryToInsert = HamtNodeEntry(std::move(leaf));
        return;
    }

    // Some loop invariants:
    //
    // - level is equal to the level entryToInsert is at, starting at 0 if it
    //   is in the top-level node.
    // - hash has been advanced `level` times.
    // - entryToInsert is the entry in which `str` belongs.
    //
    while (true) {

        unsigned lastLevel = level;
        uint64_t lastHash = hash;
        level++;

        if (UNLIKELY(level >= LEVELS_PER_HASH)
         && (level % LEVELS_PER_HASH) == 0) {
            hash = getNthBackup(str, level / LEVELS_PER_HASH - 1);
        } else {
            hash >>= BITS_PER_LEVEL;
        }

        if (!entryToInsert->isLeaf()) {
            auto nodeToInsertAt = entryToInsert->takeChild();
            int idx = nodeToInsertAt->numberOfHashesAbove(hash);

            // If there's already a child here, move into that child.
            if (nodeToInsertAt->containsHash(hash)) {
                auto nextEntry = &nodeToInsertAt->children[idx - 1];
                *entryToInsert = HamtNodeEntry(std::move(nodeToInsertAt));
                entryToInsert = nextEntry;
                continue;
            // If there's not, allocate a new node with space for one more
            // child.
            } else {
                int nChildren = nodeToInsertAt->numberOfChildren() + 1;

                auto leaf = std::make_unique<HamtLeaf>(std::move(str), hash);

                std::unique_ptr<HamtNode> newNode(
                        new (nChildren) HamtNode(std::move(nodeToInsertAt),
                                                 HamtNodeEntry(std::move(leaf)),
                                                 hash));

                *entryToInsert = HamtNodeEntry(std::move(newNode));
                return;
            }
        } else {
            auto otherLeaf = entryToInsert->takeLeaf();
            auto otherHash = otherLeaf->hash;

            if (lastHash == otherHash && str == otherLeaf->data) {
                *entryToInsert = HamtNodeEntry(std::move(otherLeaf));
                return;
            }

            if (UNLIKELY(level >= LEVELS_PER_HASH)
             && (level % LEVELS_PER_HASH) == 0) {
                otherHash = getNthBackup(otherLeaf->data,
                                         level / LEVELS_PER_HASH - 1);
            } else {
                otherHash >>= BITS_PER_LEVEL;
            }
            otherLeaf->hash = otherHash;

            std::unique_ptr<HamtNode> newNode(
                    new (1) HamtNode(otherHash,
                                     HamtNodeEntry(std::move(otherLeaf))));

            *entryToInsert = HamtNodeEntry(std::move(newNode));
            hash = lastHash;
            level = lastLevel;
            continue;
        }
    }
}

bool TopLevelHamtNode::find(uint64_t hash, const std::string &str) const {
    const HamtNodeEntry *entry = &table[hash & FIRST_N_BITS];
    uint64_t lastHash = hash;
    hash >>= BITS_PER_LEVEL;
    unsigned level = 1;

    if (entry->isNull()) return false;

    while (true) {
        if (entry->isLeaf()) {
            auto leaf = entry->getLeaf();
            return leaf.hash == lastHash && leaf.data == str;
        } else {
            const HamtNode &node = entry->getChild();

            if (!node.containsHash(hash)) {
                return false;
            }

            entry = &node.children[node.numberOfHashesAbove(hash) - 1];

            lastHash = hash;

            level++;
            if (UNLIKELY(level >= LEVELS_PER_HASH)
             && (level % LEVELS_PER_HASH) == 0) {
                hash = getNthBackup(str, level / LEVELS_PER_HASH - 1);
            } else {
                hash >>= BITS_PER_LEVEL;
            }
            continue;
        }
    }
}

void deleteFromNode(HamtNodeEntry *entry, uint64_t hash) {
    assert(entry != NULL);
    assert(!entry->isNull());

    if (entry->isLeaf()) {
        *entry = HamtNodeEntry();
    } else {
        std::unique_ptr<HamtNode> node = entry->takeChild();
        assert(node->containsHash(hash));

        int nChildren = node->numberOfChildren();

        // If we just destructed the node's only child, then delete this node
        // and be done with it:
        if (nChildren == 1) {
            return;
        }

        // Otherwise, we'll want to allocate a new, smaller node.
        std::unique_ptr<HamtNode> newNode(
                new (nChildren - 1) HamtNode(std::move(node), hash));

        *entry = HamtNodeEntry(std::move(newNode));
    }
}

bool TopLevelHamtNode::erase(uint64_t hash, const std::string &str) {
    HamtNodeEntry *entry = &table[hash & FIRST_N_BITS];
    HamtNodeEntry *entryToDeleteTo = entry;
    uint64_t hashToDeleteTo = hash >> 6;
    unsigned level = 0;

    if (entry->isNull()) return false;

    while (true) {
        level++;

        uint64_t lastHash = hash;
        if (UNLIKELY(level >= LEVELS_PER_HASH)
         && (level % LEVELS_PER_HASH) == 0) {
            hash = getNthBackup(str, level / LEVELS_PER_HASH - 1);
        } else {
            hash >>= BITS_PER_LEVEL;
        }

        if (entry->isLeaf()) {
            auto &leaf = entry->getLeaf();

            if (lastHash == leaf.hash && leaf.data == str) {
                deleteFromNode(entryToDeleteTo, hashToDeleteTo);
                return true;
            }
            return false;
        } else {
            auto &node = entry->getChild();

            if (node.numberOfChildren() > 1) {
                entryToDeleteTo = entry;
                hashToDeleteTo = hash;
            }

            if (!node.containsHash(hash)) {
                return false;
            }

            entry = &node.children[node.numberOfHashesAbove(hash) - 1];

        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// HamtNodeEntry method definitions.
//

HamtNodeEntry::HamtNodeEntry(std::unique_ptr<HamtNode> node)
    : ptr(reinterpret_cast<std::uintptr_t>(node.release())) {}

// Initialize the pointer 
HamtNodeEntry::HamtNodeEntry(std::unique_ptr<HamtLeaf> leaf)
    : ptr(reinterpret_cast<std::uintptr_t>(leaf.release()) | 1) {}

// Initialize the pointer to NULL.
HamtNodeEntry::HamtNodeEntry() : ptr(0) {}

HamtNodeEntry::HamtNodeEntry(HamtNodeEntry &&other): ptr(other.ptr) {
    other.ptr = 0;
}

HamtNodeEntry &HamtNodeEntry::operator=(HamtNodeEntry &&other) {
    this->~HamtNodeEntry();
    ptr = other.ptr;
    other.ptr = 0;
    return *this;
}

void HamtNodeEntry::release() {
    ptr = 0;
}

bool HamtNodeEntry::isLeaf() const {
    return ptr & 1;
}

bool HamtNodeEntry::isNull() const {
    return ptr == 0;
}

std::unique_ptr<HamtNode> HamtNodeEntry::takeChild() {
    assert(!isNull() && !isLeaf());
    std::unique_ptr<HamtNode> result(reinterpret_cast<HamtNode *>(ptr));
    ptr = 0;
    return result;
}

HamtNode &HamtNodeEntry::getChild() {
    assert(!isNull() && !isLeaf());
    return *reinterpret_cast<HamtNode *>(ptr);
}

const HamtNode &HamtNodeEntry::getChild() const {
    assert(!isNull() && !isLeaf());
    return *reinterpret_cast<HamtNode *>(ptr);
}

std::unique_ptr<HamtLeaf> HamtNodeEntry::takeLeaf() {
    std::unique_ptr<HamtLeaf> result(reinterpret_cast<HamtLeaf *>(ptr & (~1)));
    ptr = 0;
    return result;
}

HamtLeaf &HamtNodeEntry::getLeaf() {
    assert(isLeaf());
    return *reinterpret_cast<HamtLeaf *>(ptr & (~1));
}

const HamtLeaf &HamtNodeEntry::getLeaf() const {
    assert(isLeaf());
    return *reinterpret_cast<HamtLeaf *>(ptr & (~1));
}

HamtNodeEntry::~HamtNodeEntry() {
    if (isNull()) {
        return;
    } else if (!isLeaf()) {
        takeChild();
    } else {
        takeLeaf();
    }
}

//////////////////////////////////////////////////////////////////////////////
// HamtLeaf method definitions.
//

HamtLeaf::HamtLeaf(std::string data, uint64_t hash)
    : data(data), hash(hash) {}

//////////////////////////////////////////////////////////////////////////////
// HamtNode method definitions.
//

HamtNode::HamtNode(uint64_t hash, HamtNodeEntry entry)
    : map(1ULL << (hash & FIRST_N_BITS))
{
    new (&children[0]) HamtNodeEntry(std::move(entry));
}

HamtNode::HamtNode(uint64_t hash1, HamtNodeEntry entry1,
                   uint64_t hash2, HamtNodeEntry entry2)
{
    auto key1 = hash1 & FIRST_N_BITS;
    auto key2 = hash2 & FIRST_N_BITS;

    map = (1ULL << key1) | (1ULL << key2);

    HamtNodeEntry firstEntry;
    HamtNodeEntry secondEntry;

    if (key1 > key2) {
        new (&children[0]) HamtNodeEntry(std::move(entry1));
        new (&children[1]) HamtNodeEntry(std::move(entry2));
    } else {
        new (&children[0]) HamtNodeEntry(std::move(entry2));
        new (&children[1]) HamtNodeEntry(std::move(entry1));
    }
}

HamtNode::HamtNode(std::unique_ptr<HamtNode> node, uint64_t hash) {
    map = node->map;
    node->map = 0;
    unmarkHash(hash);
    int idx = numberOfHashesAbove(hash);
    size_t nChildren = numberOfChildren();

    // As with the corresponding constructor for insert, measurements show that
    // this is actually substantially faster than just moving the children
    // one by one.

    // memcpy and then zero out the bytes before the child we're deleting.
    size_t firstHalfBytes = idx * sizeof(HamtNodeEntry);
    std::memcpy(&children[0],
                &node->children[0],
                firstHalfBytes);
    std::memset(&node->children[0], 0, firstHalfBytes);

    // Delete the actual child we're looking at.
    node->children[idx] = HamtNodeEntry();

    // memcpy and then zero out the bytes after the child we're deleting.
    size_t sndHalfBytes = (nChildren - idx) * sizeof(HamtNodeEntry);
    std::memcpy(&children[idx],
                &node->children[idx + 1],
                sndHalfBytes);
    std::memset(&node->children[0], 0, sndHalfBytes);
}


HamtNode::HamtNode(std::unique_ptr<HamtNode> node,
                   HamtNodeEntry entry,
                   uint64_t hash) {
    uint64_t nChildren = node->numberOfChildren();
    map = node->map;
    node->map = 0;
    assert(!containsHash(hash));
    size_t idx = numberOfHashesAbove(hash);
    markHash(hash);
    std::memcpy(&children[0],
                &node->children[0],
                idx * sizeof(HamtNodeEntry));

    new (&children[idx]) HamtNodeEntry(std::move(entry));

    std::memcpy(&children[idx + 1],
                &node->children[idx],
                (nChildren - idx) * sizeof(HamtNodeEntry));

    std::memset(&node->children[0], 0,
                sizeof(HamtNodeEntry) * nChildren);
}

int HamtNode::numberOfChildren() const {
    return __builtin_popcountll((unsigned long long)map);
}

uint64_t HamtNode::numberOfHashesAbove(uint64_t hash) const {
    uint64_t rest = map >> (hash & FIRST_N_BITS);
    return __builtin_popcountll((unsigned long long)rest);
}

bool HamtNode::containsHash(uint64_t hash) const {
    return (map & (1ULL << (hash & FIRST_N_BITS))) != 0;
}

void HamtNode::markHash(uint64_t hash) {
    map |= (1ULL << (hash & FIRST_N_BITS));
}

void HamtNode::unmarkHash(uint64_t hash) {
    map &= ~(1ULL << (hash & FIRST_N_BITS));
}

HamtNode::~HamtNode() {
    int nChildren = numberOfChildren();
    for (int i = 0; i < nChildren; ++i) {
        children[i].~HamtNodeEntry();
    }
}

void *HamtNode::operator new(size_t, int nChildren) {
    return malloc(sizeof(HamtNode) + (nChildren - 1) * sizeof(HamtNodeEntry));
}

void HamtNode::operator delete(void *p) {
    free(p);
}

//////////////////////////////////////////////////////////////////////////////
// Hamt method definitions.
//

void Hamt::insert(std::string &&str) {
    uint64_t hash = hasher(str);
    root.insert(hash, std::move(str));
}

bool Hamt::find(const std::string &str) const {
    uint64_t hash = hasher(str);
    return root.find(hash, str);
}

bool Hamt::erase(const std::string &str) {
    uint64_t hash = hasher(str);
    return root.erase(hash, str);
}

// Re-enable the warning we disabled at the start.
// warning.
#ifdef __GNUC__
#ifndef __clang__
    #pragma GCC diagnostic warning "-Wclass-memaccess"
#endif
#endif
