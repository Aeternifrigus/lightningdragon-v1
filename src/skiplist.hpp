#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace velocitydb {

struct LookupResult {
    enum class State { NotFound, Found, Deleted };
    State state = State::NotFound;
    std::string value;
    uint64_t sequence = 0;
};

// Skip list used as the memtable.
//
// Nodes are linked with compare-and-swap and never unlinked, so readers need no
// locks and nothing has to be reclaimed while the list is alive. Each key keeps
// a chain of versions sorted newest first; a snapshot read walks the chain to
// the newest version at or below its sequence number. A delete is a version
// with the deleted flag set.
class SkipList {
public:
    static constexpr int kMaxHeight = 12;

    struct Version {
        std::string value;
        uint64_t sequence = 0;
        bool deleted = false;
        std::atomic<Version*> older{nullptr};
    };

private:
    struct Node {
        std::string key;
        std::atomic<Version*> versions{nullptr};
        std::atomic<Node*> next[kMaxHeight] = {};
    };

public:
    SkipList();
    ~SkipList();
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    void put(std::string_view key, std::string_view value, uint64_t sequence);
    void remove(std::string_view key, uint64_t sequence);
    LookupResult get(std::string_view key, uint64_t snapshot = UINT64_MAX) const;

    size_t key_count() const { return key_count_.load(std::memory_order_relaxed); }
    size_t memory_usage() const { return memory_usage_.load(std::memory_order_relaxed); }

    // Walks keys in order. Only safe once writes to the list have stopped.
    class Iterator {
    public:
        bool valid() const { return node_ != nullptr; }
        void next() { node_ = node_->next[0].load(std::memory_order_acquire); }
        const std::string& key() const { return node_->key; }
        const Version& newest() const { return *node_->versions.load(std::memory_order_acquire); }

    private:
        friend class SkipList;
        explicit Iterator(const Node* node) : node_(node) {}
        const Node* node_;
    };
    Iterator begin() const { return Iterator(head_->next[0].load(std::memory_order_acquire)); }

private:
    void add_version(std::string_view key, Version* version);
    void insert_version(Node* node, Version* version);
    Node* find(std::string_view key, Node** preds, Node** succs) const;
    static int random_height();

    Node* head_;
    std::atomic<size_t> key_count_{0};
    std::atomic<size_t> memory_usage_{0};
};

}  // namespace velocitydb
