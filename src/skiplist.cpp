#include "skiplist.hpp"

#include <random>

namespace velocitydb {

SkipList::SkipList() : head_(new Node) {}

SkipList::~SkipList() {
    Node* node = head_;
    while (node) {
        Node* next = node->next[0].load(std::memory_order_relaxed);
        Version* v = node->versions.load(std::memory_order_relaxed);
        while (v) {
            Version* older = v->older.load(std::memory_order_relaxed);
            delete v;
            v = older;
        }
        delete node;
        node = next;
    }
}

int SkipList::random_height() {
    thread_local std::mt19937 gen(std::random_device{}());
    int height = 1;
    // each extra level with probability 1/4
    while (height < kMaxHeight && (gen() & 3) == 0) height++;
    return height;
}

// Fills preds/succs for every level and returns the node holding `key`, if any.
SkipList::Node* SkipList::find(std::string_view key, Node** preds, Node** succs) const {
    Node* pred = head_;
    for (int level = kMaxHeight - 1; level >= 0; level--) {
        Node* curr = pred->next[level].load(std::memory_order_acquire);
        while (curr && curr->key < key) {
            pred = curr;
            curr = curr->next[level].load(std::memory_order_acquire);
        }
        if (preds) preds[level] = pred;
        if (succs) succs[level] = curr;
        if (level == 0) return (curr && curr->key == key) ? curr : nullptr;
    }
    return nullptr;
}

// Keeps the chain sorted by sequence, newest first. Normally the new version
// goes at the head; the walk only matters if writes arrive out of order.
void SkipList::insert_version(Node* node, Version* version) {
    std::atomic<Version*>* link = &node->versions;
    while (true) {
        Version* current = link->load(std::memory_order_acquire);
        if (current == nullptr || current->sequence < version->sequence) {
            version->older.store(current, std::memory_order_relaxed);
            if (link->compare_exchange_weak(current, version, std::memory_order_release,
                                            std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        link = &current->older;
    }
    memory_usage_.fetch_add(sizeof(Version) + version->value.size(), std::memory_order_relaxed);
}

void SkipList::add_version(std::string_view key, Version* version) {
    Node* preds[kMaxHeight];
    Node* succs[kMaxHeight];

    while (true) {
        if (Node* existing = find(key, preds, succs)) {
            insert_version(existing, version);
            return;
        }

        const int height = random_height();
        auto* node = new Node;
        node->key.assign(key);
        node->versions.store(version, std::memory_order_relaxed);
        for (int level = 0; level < height; level++) {
            node->next[level].store(succs[level], std::memory_order_relaxed);
        }

        // Level 0 decides whether the key is in the list. If another writer got
        // there first, start over and add to its node instead.
        Node* expected = succs[0];
        if (!preds[0]->next[0].compare_exchange_strong(expected, node, std::memory_order_release,
                                                       std::memory_order_acquire)) {
            node->versions.store(nullptr, std::memory_order_relaxed);
            delete node;
            continue;
        }

        for (int level = 1; level < height; level++) {
            while (true) {
                expected = succs[level];
                if (preds[level]->next[level].compare_exchange_strong(
                        expected, node, std::memory_order_release, std::memory_order_acquire)) {
                    break;
                }
                find(key, preds, succs);
                node->next[level].store(succs[level], std::memory_order_relaxed);
            }
        }

        key_count_.fetch_add(1, std::memory_order_relaxed);
        memory_usage_.fetch_add(sizeof(Node) + key.size() + sizeof(Version) + version->value.size(),
                                std::memory_order_relaxed);
        return;
    }
}

void SkipList::put(std::string_view key, std::string_view value, uint64_t sequence) {
    auto* v = new Version;
    v->value.assign(value);
    v->sequence = sequence;
    add_version(key, v);
}

void SkipList::remove(std::string_view key, uint64_t sequence) {
    auto* v = new Version;
    v->sequence = sequence;
    v->deleted = true;
    add_version(key, v);
}

LookupResult SkipList::get(std::string_view key, uint64_t snapshot) const {
    LookupResult result;
    const Node* node = find(key, nullptr, nullptr);
    if (!node) return result;

    for (const Version* v = node->versions.load(std::memory_order_acquire); v;
         v = v->older.load(std::memory_order_acquire)) {
        if (v->sequence <= snapshot) {
            result.state = v->deleted ? LookupResult::State::Deleted : LookupResult::State::Found;
            result.sequence = v->sequence;
            if (!v->deleted) result.value = v->value;
            return result;
        }
    }
    return result;
}

}  // namespace velocitydb
