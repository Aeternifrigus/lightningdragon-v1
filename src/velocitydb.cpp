/**
 * VelocityDB Implementation
 * High-Performance Database Engine
 */

#include "velocitydb.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#ifdef __unix__
#include <unistd.h>
#endif

namespace velocitydb {

// ============================================================================
// Lock-Free Skip List Implementation (Simplified for stability)
// ============================================================================

template<typename K, typename V>
LockFreeSkipList<K, V>::LockFreeSkipList() {
    head_ = new_node(K{}, V{}, 0, MAX_HEIGHT);
    for (int i = 0; i < MAX_HEIGHT; i++) {
        head_->next[i].store(nullptr, std::memory_order_relaxed);
    }
}

template<typename K, typename V>
LockFreeSkipList<K, V>::~LockFreeSkipList() {
    Node* current = head_->next[0].load(std::memory_order_relaxed);
    while (current) {
        Node* next = current->next[0].load(std::memory_order_relaxed);
        current->~Node();
        ::operator delete(current);
        current = next;
    }
    head_->~Node();
    ::operator delete(head_);
}

template<typename K, typename V>
typename LockFreeSkipList<K, V>::Node* 
LockFreeSkipList<K, V>::new_node(const K& key, const V& value, uint64_t seq, int height) {
    size_t size = sizeof(Node) + (height) * sizeof(std::atomic<Node*>);
    void* mem = ::operator new(size);
    Node* node = new (mem) Node();
    node->key = key;
    node->value = value;
    node->sequence = seq;
    node->deleted = false;
    for (int i = 0; i < height; i++) {
        new (&node->next[i]) std::atomic<Node*>(nullptr);
    }
    memory_usage_.fetch_add(size + key.size() + value.size(), std::memory_order_relaxed);
    return node;
}

template<typename K, typename V>
int LockFreeSkipList<K, V>::random_height() {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_real_distribution<> dis(0, 1);
    
    int height = 1;
    while (height < MAX_HEIGHT && dis(gen) < 0.25) {
        height++;
    }
    return height;
}

template<typename K, typename V>
bool LockFreeSkipList<K, V>::find_position(const K& key, Node** preds, Node** succs) {
    int max_h = max_height_.load(std::memory_order_acquire);
    Node* pred = head_;
    
    // Initialize all levels
    for (int i = 0; i < MAX_HEIGHT; i++) {
        preds[i] = head_;
        succs[i] = nullptr;
    }
    
    for (int i = max_h - 1; i >= 0; i--) {
        Node* curr = pred->next[i].load(std::memory_order_acquire);
        while (curr && curr->key < key) {
            pred = curr;
            curr = curr->next[i].load(std::memory_order_acquire);
        }
        preds[i] = pred;
        succs[i] = curr;
    }
    
    return succs[0] && succs[0]->key == key;
}

template<typename K, typename V>
bool LockFreeSkipList<K, V>::insert(const K& key, const V& value, uint64_t seq) {
    int height = random_height();
    Node* preds[MAX_HEIGHT];
    Node* succs[MAX_HEIGHT];
    
    for (int retry = 0; retry < 100; retry++) {
        bool found = find_position(key, preds, succs);
        
        if (found) {
            Node* existing = succs[0];
            if (existing && existing->sequence < seq) {
                existing->value = value;
                existing->sequence = seq;
                existing->deleted = false;
                return true;
            }
            return existing != nullptr;
        }
        
        Node* new_node_ptr = new_node(key, value, seq, height);
        
        // Update max height if needed
        int old_max_height = max_height_.load(std::memory_order_acquire);
        while (height > old_max_height) {
            if (max_height_.compare_exchange_weak(old_max_height, height,
                    std::memory_order_release, std::memory_order_acquire)) {
                // Update preds for new levels to point to head
                for (int i = old_max_height; i < height; i++) {
                    preds[i] = head_;
                    succs[i] = nullptr;
                }
                break;
            }
        }
        
        // Link next pointers first
        for (int i = 0; i < height; i++) {
            new_node_ptr->next[i].store(succs[i], std::memory_order_relaxed);
        }
        
        // CAS at level 0
        Node* expected = succs[0];
        if (!preds[0]->next[0].compare_exchange_strong(expected, new_node_ptr,
                std::memory_order_release, std::memory_order_acquire)) {
            // Failed - cleanup and retry
            delete new_node_ptr;
            continue;
        }
        
        // Link upper levels (best effort)
        for (int i = 1; i < height; i++) {
            while (true) {
                expected = succs[i];
                if (preds[i]->next[i].compare_exchange_weak(expected, new_node_ptr,
                        std::memory_order_release, std::memory_order_acquire)) {
                    break;
                }
                // Refind position for this level
                find_position(key, preds, succs);
                if (succs[i] == new_node_ptr) break;  // Already linked
            }
        }
        
        size_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

template<typename K, typename V>
std::optional<V> LockFreeSkipList<K, V>::find(const K& key, uint64_t snapshot_seq) const {
    int max_h = max_height_.load(std::memory_order_acquire);
    const Node* pred = head_;
    
    for (int i = max_h - 1; i >= 0; i--) {
        const Node* curr = pred->next[i].load(std::memory_order_acquire);
        while (curr && curr->key < key) {
            pred = curr;
            curr = curr->next[i].load(std::memory_order_acquire);
        }
    }
    
    const Node* found = pred->next[0].load(std::memory_order_acquire);
    if (found && found->key == key && found->sequence <= snapshot_seq && !found->deleted) {
        return found->value;
    }
    return std::nullopt;
}

template<typename K, typename V>
bool LockFreeSkipList<K, V>::remove(const K& key, uint64_t seq) {
    Node* preds[MAX_HEIGHT];
    Node* succs[MAX_HEIGHT];
    
    bool found = find_position(key, preds, succs);
    if (!found || !succs[0]) return false;
    
    Node* target = succs[0];
    target->deleted = true;
    target->sequence = seq;
    return true;
}

// Iterator implementation
template<typename K, typename V>
LockFreeSkipList<K, V>::Iterator::Iterator(const LockFreeSkipList* list) 
    : list_(list), current_(nullptr) {
    if (list_ && list_->head_) {
        current_ = list_->head_->next[0].load(std::memory_order_acquire);
    }
}

template<typename K, typename V>
bool LockFreeSkipList<K, V>::Iterator::valid() const {
    return current_ != nullptr;
}

template<typename K, typename V>
void LockFreeSkipList<K, V>::Iterator::next() {
    if (current_) {
        current_ = current_->next[0].load(std::memory_order_acquire);
    }
}

template<typename K, typename V>
const K& LockFreeSkipList<K, V>::Iterator::key() const {
    return current_->key;
}

template<typename K, typename V>
const V& LockFreeSkipList<K, V>::Iterator::value() const {
    return current_->value;
}

template<typename K, typename V>
uint64_t LockFreeSkipList<K, V>::Iterator::sequence() const {
    return current_->sequence;
}

template<typename K, typename V>
bool LockFreeSkipList<K, V>::Iterator::deleted() const {
    return current_->deleted;
}

// Explicit instantiation
template class LockFreeSkipList<std::string, std::string>;

// ============================================================================
// WAL Implementation
// ============================================================================

WAL::WAL(const std::string& path) : path_(path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    file_.open(path, std::ios::binary | std::ios::app);
}

WAL::~WAL() {
    if (file_.is_open()) {
        file_.close();
    }
}

void WAL::append(const std::string& key, const std::string& value, uint64_t seq, bool deleted) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t value_len = static_cast<uint32_t>(value.size());
    uint8_t del = deleted ? 1 : 0;
    
    file_.write(reinterpret_cast<const char*>(&key_len), 4);
    file_.write(key.data(), key_len);
    file_.write(reinterpret_cast<const char*>(&value_len), 4);
    file_.write(value.data(), value_len);
    file_.write(reinterpret_cast<const char*>(&seq), 8);
    file_.write(reinterpret_cast<const char*>(&del), 1);
    
    bytes_written_ += 4 + key_len + 4 + value_len + 8 + 1;
}

void WAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
    // Force sync to disk for durability
    // Note: For true durability, use platform-specific fsync
    #ifdef __unix__
    if (file_.is_open()) {
        ::sync();  // Sync all filesystems
    }
    #endif
}

void WAL::rotate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
    
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    std::error_code ec;
    std::filesystem::rename(path_, path_ + "." + std::to_string(timestamp), ec);
    
    file_.open(path_, std::ios::binary | std::ios::app);
    bytes_written_ = 0;
}

void WAL::recover(std::function<void(const std::string&, const std::string&, uint64_t, bool)> callback) {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    
    while (in) {
        uint32_t key_len;
        if (!in.read(reinterpret_cast<char*>(&key_len), 4)) break;
        
        std::string key(key_len, '\0');
        if (!in.read(key.data(), key_len)) break;
        
        uint32_t value_len;
        if (!in.read(reinterpret_cast<char*>(&value_len), 4)) break;
        
        std::string value(value_len, '\0');
        if (!in.read(value.data(), value_len)) break;
        
        uint64_t seq;
        if (!in.read(reinterpret_cast<char*>(&seq), 8)) break;
        
        uint8_t deleted;
        if (!in.read(reinterpret_cast<char*>(&deleted), 1)) break;
        
        callback(key, value, seq, deleted != 0);
    }
}

// ============================================================================
// Compressor Implementation (LZ4-style)
// ============================================================================

std::vector<uint8_t> Compressor::compress(const uint8_t* data, size_t size) {
    if (size == 0) return {};
    
    std::vector<uint8_t> output;
    output.reserve(size / 4);
    
    // Store original size first (big-endian)
    output.push_back((size >> 24) & 0xFF);
    output.push_back((size >> 16) & 0xFF);
    output.push_back((size >> 8) & 0xFF);
    output.push_back(size & 0xFF);
    
    // LZ77-style compression with sliding window
    std::unordered_map<uint32_t, std::vector<size_t>> hash_table;
    
    auto hash4 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };
    
    size_t i = 0;
    while (i < size) {
        size_t best_length = 0;
        size_t best_offset = 0;
        
        // Try to find a match (need at least 4 bytes)
        if (i + 4 <= size) {
            uint32_t h = hash4(data + i);
            auto it = hash_table.find(h);
            
            if (it != hash_table.end()) {
                for (size_t pos : it->second) {
                    if (i - pos > 65535) continue;  // Max offset
                    
                    size_t len = 0;
                    while (i + len < size && len < 255 && data[pos + len] == data[i + len]) {
                        len++;
                    }
                    
                    if (len > best_length && len >= 4) {
                        best_length = len;
                        best_offset = i - pos;
                    }
                }
            }
            
            hash_table[h].push_back(i);
            // Keep hash table small
            if (hash_table[h].size() > 8) {
                hash_table[h].erase(hash_table[h].begin());
            }
        }
        
        if (best_length >= 4) {
            // Match found: [0xFF][length][offset_hi][offset_lo]
            output.push_back(0xFF);
            output.push_back(static_cast<uint8_t>(best_length));
            output.push_back(static_cast<uint8_t>(best_offset >> 8));
            output.push_back(static_cast<uint8_t>(best_offset & 0xFF));
            i += best_length;
        } else {
            // Check for RLE
            size_t run_length = 1;
            while (i + run_length < size && 
                   data[i + run_length] == data[i] && 
                   run_length < 255) {
                run_length++;
            }
            
            if (run_length >= 4) {
                output.push_back(0xFE);
                output.push_back(static_cast<uint8_t>(run_length));
                output.push_back(data[i]);
                i += run_length;
            } else {
                // Literal
                if (data[i] >= 0xFD) {
                    output.push_back(0xFD);
                }
                output.push_back(data[i]);
                i++;
            }
        }
    }
    
    return output;
}

std::vector<uint8_t> Compressor::decompress(const uint8_t* data, size_t size) {
    if (size < 4) return {};
    
    size_t original_size = (static_cast<size_t>(data[0]) << 24) |
                           (static_cast<size_t>(data[1]) << 16) |
                           (static_cast<size_t>(data[2]) << 8) |
                           static_cast<size_t>(data[3]);
    
    std::vector<uint8_t> output;
    output.reserve(original_size);
    
    size_t i = 4;
    while (i < size && output.size() < original_size) {
        if (data[i] == 0xFF && i + 3 < size) {
            // LZ77 match: [0xFF][length][offset_hi][offset_lo]
            size_t length = data[i + 1];
            size_t offset = (static_cast<size_t>(data[i + 2]) << 8) | data[i + 3];
            
            // Bounds check: ensure offset is valid
            if (offset > 0 && offset <= output.size()) {
                size_t match_pos = output.size() - offset;
                for (size_t j = 0; j < length && output.size() < original_size; j++) {
                    output.push_back(output[match_pos + j]);
                }
            }
            i += 4;
        } else if (data[i] == 0xFE && i + 2 < size) {
            // RLE
            size_t run_length = data[i + 1];
            uint8_t byte = data[i + 2];
            for (size_t j = 0; j < run_length && output.size() < original_size; j++) {
                output.push_back(byte);
            }
            i += 3;
        } else if (data[i] == 0xFD && i + 1 < size) {
            // Escaped literal
            output.push_back(data[i + 1]);
            i += 2;
        } else {
            output.push_back(data[i]);
            i++;
        }
    }
    
    return output;
}

// ============================================================================
// Bloom Filter Implementation
// ============================================================================

BloomFilter::BloomFilter(size_t expected_items, size_t bits_per_key) {
    size_t num_bits = expected_items * bits_per_key;
    num_bits = std::max(num_bits, size_t(64));
    bits_.resize((num_bits + 7) / 8, 0);
    num_hashes_ = std::max(size_t(1), std::min(size_t(30), bits_per_key * 69 / 100));
}

std::array<uint64_t, 2> BloomFilter::hash(std::string_view key) const {
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 0xcbf29ce484222325ULL;
    
    for (char c : key) {
        h1 ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h1 *= 1099511628211ULL;
        h2 ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h2 *= 0x100000001b3ULL;
    }
    
    return {h1, h2};
}

void BloomFilter::add(std::string_view key) {
    auto [h1, h2] = hash(key);
    size_t num_bits = bits_.size() * 8;
    
    for (size_t i = 0; i < num_hashes_; i++) {
        size_t bit = (h1 + i * h2) % num_bits;
        bits_[bit / 8] |= (1 << (bit % 8));
    }
}

bool BloomFilter::may_contain(std::string_view key) const {
    auto [h1, h2] = hash(key);
    size_t num_bits = bits_.size() * 8;
    
    for (size_t i = 0; i < num_hashes_; i++) {
        size_t bit = (h1 + i * h2) % num_bits;
        if (!(bits_[bit / 8] & (1 << (bit % 8)))) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// SSTable Implementation
// ============================================================================

SSTable::SSTable(const std::string& path) : path_(path) {}

std::unique_ptr<SSTable> SSTable::create(
    const std::string& path,
    const std::vector<std::tuple<std::string, std::string, uint64_t, bool>>& entries,
    bool compress) {
    
    if (entries.empty()) return nullptr;
    
    auto sst = std::unique_ptr<SSTable>(new SSTable(path));
    
    std::ofstream file(path, std::ios::binary);
    if (!file) return nullptr;
    
    sst->bloom_ = std::make_unique<BloomFilter>(entries.size(), 10);
    for (const auto& [key, value, seq, deleted] : entries) {
        sst->bloom_->add(key);
    }
    
    std::vector<uint8_t> data_buffer;
    uint64_t min_seq = UINT64_MAX;
    uint64_t max_seq = 0;
    
    for (const auto& [key, value, seq, deleted] : entries) {
        min_seq = std::min(min_seq, seq);
        max_seq = std::max(max_seq, seq);
        
        sst->index_.emplace_back(key, data_buffer.size());
        
        uint32_t key_len = static_cast<uint32_t>(key.size());
        uint32_t value_len = static_cast<uint32_t>(value.size());
        uint8_t del = deleted ? 1 : 0;
        
        data_buffer.insert(data_buffer.end(), 
            reinterpret_cast<const uint8_t*>(&key_len),
            reinterpret_cast<const uint8_t*>(&key_len) + 4);
        data_buffer.insert(data_buffer.end(), key.begin(), key.end());
        data_buffer.insert(data_buffer.end(),
            reinterpret_cast<const uint8_t*>(&value_len),
            reinterpret_cast<const uint8_t*>(&value_len) + 4);
        data_buffer.insert(data_buffer.end(), value.begin(), value.end());
        uint64_t seq_copy = seq;
        data_buffer.insert(data_buffer.end(),
            reinterpret_cast<const uint8_t*>(&seq_copy),
            reinterpret_cast<const uint8_t*>(&seq_copy) + 8);
        data_buffer.push_back(del);
    }
    
    std::vector<uint8_t> final_data;
    if (compress) {
        final_data = Compressor::compress(data_buffer.data(), data_buffer.size());
    } else {
        final_data = std::move(data_buffer);
    }
    
    sst->footer_.data_offset = 0;
    sst->footer_.data_size = final_data.size();
    file.write(reinterpret_cast<const char*>(final_data.data()), final_data.size());
    
    sst->footer_.index_offset = file.tellp();
    for (const auto& [key, offset] : sst->index_) {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        file.write(reinterpret_cast<const char*>(&key_len), 4);
        file.write(key.data(), key_len);
        file.write(reinterpret_cast<const char*>(&offset), 8);
    }
    sst->footer_.index_size = static_cast<uint64_t>(file.tellp()) - sst->footer_.index_offset;
    
    sst->footer_.bloom_offset = file.tellp();
    const auto& bloom_data = sst->bloom_->data();
    file.write(reinterpret_cast<const char*>(bloom_data.data()), bloom_data.size());
    sst->footer_.bloom_size = bloom_data.size();
    
    sst->footer_.min_sequence = min_seq;
    sst->footer_.max_sequence = max_seq;
    sst->footer_.num_entries = static_cast<uint32_t>(entries.size());
    sst->footer_.magic = 0xDB12CAFE;
    file.write(reinterpret_cast<const char*>(&sst->footer_), sizeof(Footer));
    
    file.close();
    sst->file_.open(path, std::ios::binary);
    
    return sst;
}

std::unique_ptr<SSTable> SSTable::open(const std::string& path) {
    auto sst = std::unique_ptr<SSTable>(new SSTable(path));
    
    sst->file_.open(path, std::ios::binary);
    if (!sst->file_) return nullptr;
    
    sst->file_.seekg(-static_cast<int>(sizeof(Footer)), std::ios::end);
    sst->file_.read(reinterpret_cast<char*>(&sst->footer_), sizeof(Footer));
    
    if (sst->footer_.magic != 0xDB12CAFE) return nullptr;
    
    sst->file_.seekg(sst->footer_.bloom_offset);
    std::vector<uint8_t> bloom_data(sst->footer_.bloom_size);
    sst->file_.read(reinterpret_cast<char*>(bloom_data.data()), bloom_data.size());
    sst->bloom_ = std::make_unique<BloomFilter>(sst->footer_.num_entries, 10);
    
    sst->file_.seekg(sst->footer_.index_offset);
    while (sst->file_.tellg() < static_cast<std::streampos>(sst->footer_.bloom_offset)) {
        uint32_t key_len;
        if (!sst->file_.read(reinterpret_cast<char*>(&key_len), 4)) break;
        
        std::string key(key_len, '\0');
        if (!sst->file_.read(key.data(), key_len)) break;
        
        uint64_t offset;
        if (!sst->file_.read(reinterpret_cast<char*>(&offset), 8)) break;
        
        sst->index_.emplace_back(std::move(key), offset);
    }
    
    return sst;
}

std::optional<std::string> SSTable::get(std::string_view key, uint64_t snapshot_seq) {
    if (!bloom_->may_contain(key)) {
        return std::nullopt;
    }
    
    std::string key_str(key);
    auto it = std::lower_bound(index_.begin(), index_.end(), 
        std::make_pair(key_str, uint64_t(0)),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    
    if (it == index_.end() || it->first != key_str) {
        return std::nullopt;
    }
    
    std::lock_guard<std::mutex> lock(file_mutex_);
    
    file_.seekg(footer_.data_offset);
    std::vector<uint8_t> compressed_data(footer_.data_size);
    file_.read(reinterpret_cast<char*>(compressed_data.data()), compressed_data.size());
    
    auto data = Compressor::decompress(compressed_data.data(), compressed_data.size());
    
    size_t offset = it->second;
    if (offset >= data.size()) return std::nullopt;
    
    uint32_t key_len;
    std::memcpy(&key_len, data.data() + offset, 4);
    offset += 4;
    
    if (offset + key_len > data.size()) return std::nullopt;
    std::string found_key(data.begin() + offset, data.begin() + offset + key_len);
    offset += key_len;
    
    if (offset + 4 > data.size()) return std::nullopt;
    uint32_t value_len;
    std::memcpy(&value_len, data.data() + offset, 4);
    offset += 4;
    
    if (offset + value_len > data.size()) return std::nullopt;
    std::string value(data.begin() + offset, data.begin() + offset + value_len);
    offset += value_len;
    
    if (offset + 9 > data.size()) return std::nullopt;
    uint64_t seq;
    std::memcpy(&seq, data.data() + offset, 8);
    offset += 8;
    
    bool deleted = data[offset] != 0;
    
    if (seq <= snapshot_seq && !deleted) {
        return value;
    }
    
    return std::nullopt;
}

// ============================================================================
// Snapshot Manager Implementation
// ============================================================================

Snapshot SnapshotManager::create_snapshot() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    uint64_t seq = current_seq_.fetch_add(1, std::memory_order_relaxed);
    active_snapshots_.push_back(seq);
    return Snapshot{seq, std::chrono::steady_clock::now()};
}

void SnapshotManager::release_snapshot(uint64_t seq) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = std::find(active_snapshots_.begin(), active_snapshots_.end(), seq);
    if (it != active_snapshots_.end()) {
        active_snapshots_.erase(it);
    }
}

uint64_t SnapshotManager::oldest_snapshot_seq() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (active_snapshots_.empty()) {
        return current_seq_.load(std::memory_order_relaxed);
    }
    return *std::min_element(active_snapshots_.begin(), active_snapshots_.end());
}

// ============================================================================
// Transaction Implementation
// ============================================================================

Transaction::Transaction(VelocityDB* db, uint64_t snapshot_seq)
    : db_(db), snapshot_seq_(snapshot_seq) {}

Transaction::~Transaction() {
    if (!committed_ && !rolledback_) {
        rollback();
    }
}

void Transaction::put(std::string_view key, std::string_view value) {
    writes_.emplace_back(std::string(key), std::string(value), false);
}

std::optional<std::string> Transaction::get(std::string_view key) {
    for (auto it = writes_.rbegin(); it != writes_.rend(); ++it) {
        if (std::get<0>(*it) == key) {
            if (std::get<2>(*it)) {
                return std::nullopt;
            }
            return std::get<1>(*it);
        }
    }
    
    return db_->get_at_snapshot(key, Snapshot{snapshot_seq_, {}});
}

void Transaction::remove(std::string_view key) {
    writes_.emplace_back(std::string(key), "", true);
}

bool Transaction::commit() {
    if (committed_ || rolledback_) return false;
    
    for (const auto& [key, value, is_delete] : writes_) {
        if (is_delete) {
            db_->remove(key);
        } else {
            db_->put(key, value);
        }
    }
    
    committed_ = true;
    return true;
}

void Transaction::rollback() {
    rolledback_ = true;
    writes_.clear();
}

// ============================================================================
// VelocityDB Implementation
// ============================================================================

VelocityDB::VelocityDB(const Config& config) : config_(config) {
    std::filesystem::create_directories(config_.data_dir);
    
    memtable_ = std::make_unique<LockFreeSkipList<std::string, std::string>>();
    
    if (config_.enable_wal) {
        wal_ = std::make_unique<WAL>(config_.data_dir + "/wal.log");
        
        wal_->recover([this](const std::string& key, const std::string& value, 
                            uint64_t seq, bool deleted) {
            if (seq >= sequence_.load()) {
                sequence_.store(seq + 1);
            }
            if (deleted) {
                memtable_->remove(key, seq);
            } else {
                memtable_->insert(key, value, seq);
            }
        });
    }
    
    levels_.resize(7);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(config_.data_dir, ec)) {
        if (entry.path().extension() == ".sst") {
            auto sst = SSTable::open(entry.path().string());
            if (sst) {
                levels_[0].push_back(std::move(sst));
            }
        }
    }
    
    flush_thread_ = std::thread(&VelocityDB::background_flush, this);
    compaction_thread_ = std::thread(&VelocityDB::background_compact, this);
}

VelocityDB::~VelocityDB() {
    running_ = false;
    flush_cv_.notify_all();
    
    if (flush_thread_.joinable()) flush_thread_.join();
    if (compaction_thread_.joinable()) compaction_thread_.join();
    
    if (memtable_ && memtable_->size() > 0) {
        flush_memtable();
    }
}

bool VelocityDB::put(std::string_view key, std::string_view value) {
    auto start = std::chrono::high_resolution_clock::now();
    
    uint64_t seq = next_sequence();
    
    if (wal_) {
        wal_->append(std::string(key), std::string(value), seq, false);
    }
    
    {
        std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
        memtable_->insert(std::string(key), std::string(value), seq);
    }
    
    stats_.writes.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_written.fetch_add(key.size() + value.size(), std::memory_order_relaxed);
    
    maybe_schedule_flush();
    
    auto end = std::chrono::high_resolution_clock::now();
    last_write_latency_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    return true;
}

std::optional<std::string> VelocityDB::get(std::string_view key) {
    auto start = std::chrono::high_resolution_clock::now();
    
    {
        std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
        auto result = memtable_->find(std::string(key));
        if (result) {
            stats_.reads.fetch_add(1, std::memory_order_relaxed);
            stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
            
            auto end = std::chrono::high_resolution_clock::now();
            last_read_latency_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            
            return result;
        }
        
        if (immutable_memtable_) {
            result = immutable_memtable_->find(std::string(key));
            if (result) {
                stats_.reads.fetch_add(1, std::memory_order_relaxed);
                stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
                
                auto end = std::chrono::high_resolution_clock::now();
                last_read_latency_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                
                return result;
            }
        }
    }
    
    {
        std::shared_lock<std::shared_mutex> lock(levels_mutex_);
        for (const auto& level : levels_) {
            for (const auto& sst : level) {
                auto result = sst->get(key);
                if (result) {
                    stats_.reads.fetch_add(1, std::memory_order_relaxed);
                    stats_.cache_misses.fetch_add(1, std::memory_order_relaxed);
                    
                    auto end = std::chrono::high_resolution_clock::now();
                    last_read_latency_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                    
                    return result;
                }
            }
        }
    }
    
    stats_.reads.fetch_add(1, std::memory_order_relaxed);
    stats_.cache_misses.fetch_add(1, std::memory_order_relaxed);
    
    auto end = std::chrono::high_resolution_clock::now();
    last_read_latency_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    return std::nullopt;
}

bool VelocityDB::remove(std::string_view key) {
    uint64_t seq = next_sequence();
    
    if (wal_) {
        wal_->append(std::string(key), "", seq, true);
    }
    
    std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
    return memtable_->remove(std::string(key), seq);
}

bool VelocityDB::put_batch(const std::vector<std::pair<std::string, std::string>>& kvs) {
    for (const auto& [key, value] : kvs) {
        put(key, value);
    }
    return true;
}

std::unique_ptr<Transaction> VelocityDB::begin_transaction() {
    auto snapshot = create_snapshot();
    return std::make_unique<Transaction>(this, snapshot.sequence);
}

Snapshot VelocityDB::create_snapshot() {
    return snapshot_manager_.create_snapshot();
}

std::optional<std::string> VelocityDB::get_at_snapshot(std::string_view key, const Snapshot& snapshot) {
    std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
    return memtable_->find(std::string(key), snapshot.sequence);
}

void VelocityDB::release_snapshot(const Snapshot& snapshot) {
    snapshot_manager_.release_snapshot(snapshot.sequence);
}

void VelocityDB::maybe_schedule_flush() {
    if (memtable_->memory_usage() >= config_.memtable_size_limit) {
        needs_flush_ = true;
        flush_cv_.notify_one();
    }
}

void VelocityDB::background_flush() {
    while (running_) {
        std::unique_lock<std::mutex> lock(flush_mutex_);
        flush_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return needs_flush_.load() || !running_.load(); });
        
        if (!running_) break;
        
        if (needs_flush_) {
            needs_flush_ = false;
            lock.unlock();
            flush_memtable();
        }
    }
}

void VelocityDB::flush_memtable() {
    {
        std::unique_lock<std::shared_mutex> lock(memtable_mutex_);
        immutable_memtable_ = std::move(memtable_);
        memtable_ = std::make_unique<LockFreeSkipList<std::string, std::string>>();
    }
    
    if (!immutable_memtable_ || immutable_memtable_->size() == 0) {
        return;
    }
    
    std::vector<std::tuple<std::string, std::string, uint64_t, bool>> entries;
    for (auto it = immutable_memtable_->begin(); it.valid(); it.next()) {
        entries.emplace_back(it.key(), it.value(), it.sequence(), it.deleted());
    }
    
    std::sort(entries.begin(), entries.end(), 
        [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string sst_path = config_.data_dir + "/L0_" + std::to_string(timestamp) + ".sst";
    
    auto sst = SSTable::create(sst_path, entries, config_.enable_compression);
    if (sst) {
        std::unique_lock<std::shared_mutex> lock(levels_mutex_);
        levels_[0].push_back(std::move(sst));
    }
    
    if (wal_) {
        wal_->rotate();
    }
    
    {
        std::unique_lock<std::shared_mutex> lock(memtable_mutex_);
        immutable_memtable_.reset();
    }
}

void VelocityDB::background_compact() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        if (!running_) break;
        
        compact();
    }
}

void VelocityDB::compact() {
    std::unique_lock<std::shared_mutex> lock(levels_mutex_);
    
    if (levels_[0].size() < 4) return;
    
    // Simplified compaction stub
}

void VelocityDB::flush() {
    if (memtable_ && memtable_->size() > 0) {
        needs_flush_ = true;
        flush_cv_.notify_one();
    }
    if (wal_) {
        wal_->sync();
    }
}

}  // namespace velocitydb
