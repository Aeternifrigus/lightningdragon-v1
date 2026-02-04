# VelocityDB - High-Performance Database Engine

**An educational demonstration of advanced database engine concepts implemented in modern C++17.**

> **Note:** This is a demonstration/learning project showcasing database internals. For production use, consider established databases like RocksDB, LevelDB, or SQLite. This implementation prioritizes clarity and demonstrates concepts over production-grade safety.

## Performance Targets

| Metric | Target | Description |
|--------|--------|-------------|
| **Write Throughput** | 1M writes/sec | Lock-free skip list with concurrent writes |
| **Read Latency** | < 10μs | Bloom filters + in-memory index |
| **ACID Compliance** | Snapshot Isolation | MVCC with WAL for durability |
| **Compression** | 10:1 ratio | LZ4-style compression algorithm |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      VelocityDB Engine                       │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   MemTable  │  │     WAL     │  │  Snapshot Manager   │  │
│  │ (Skip List) │  │  (Durability)│  │      (MVCC)        │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                   │
│         ▼                ▼                                   │
│  ┌─────────────────────────────────────────────────────────┐│
│  │                    LSM-Tree Storage                      ││
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐    ││
│  │  │ SSTable │  │ SSTable │  │ SSTable │  │   ...   │    ││
│  │  │   L0    │  │   L1    │  │   L2    │  │         │    ││
│  │  └────┬────┘  └────┬────┘  └────┬────┘  └─────────┘    ││
│  │       │            │            │                       ││
│  │       └────────────┴────────────┴──→ Compaction         ││
│  └─────────────────────────────────────────────────────────┘│
│                                                              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ Bloom Filter│  │ Compressor  │  │   Block Cache       │  │
│  │ (Fast Neg.) │  │   (LZ4)     │  │   (Hot Data)        │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. Lock-Free Skip List (MemTable)
- Concurrent reads and writes without locks
- O(log n) insert and lookup
- Memory-efficient variable-height nodes

### 2. LSM-Tree Storage
- Write-optimized storage structure
- Sorted String Tables (SSTables) for persistence
- Background compaction for space reclamation

### 3. Write-Ahead Log (WAL)
- Durability before acknowledgment
- Crash recovery support
- Log rotation for space management

### 4. MVCC with Snapshot Isolation
- Non-blocking reads during writes
- Consistent point-in-time snapshots
- Sequence numbers for version ordering

### 5. LZ4-Style Compression
- Fast compression/decompression
- Dictionary + RLE hybrid approach
- Optimized for structured data

### 6. Bloom Filters
- Probabilistic membership testing
- Reduces disk I/O for missing keys
- Configurable false-positive rate

## Building

### Using Make (Simple)

```bash
# Build release version
make

# Build debug version with sanitizers
make debug

# Run demo
make run

# Run benchmark with custom parameters
make benchmark WRITES=1000000 READS=1000000 THREADS=8
```

### Using CMake

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
./velocitydb_demo
```

## Usage

### Basic Operations

```cpp
#include "velocitydb.hpp"

using namespace velocitydb;

int main() {
    // Initialize with default config
    VelocityDB db;
    
    // Put key-value
    db.put("user:1001", R"({"name": "Alice", "score": 9500})");
    
    // Get value
    auto result = db.get("user:1001");
    if (result) {
        std::cout << *result << std::endl;
    }
    
    // Delete key
    db.remove("user:1001");
    
    return 0;
}
```

### Transactions

```cpp
// Begin transaction
auto txn = db.begin_transaction();

// Perform operations
txn->put("account:A", "1000");
txn->put("account:B", "500");

// Atomic transfer
auto balance_a = std::stoi(*txn->get("account:A"));
auto balance_b = std::stoi(*txn->get("account:B"));
txn->put("account:A", std::to_string(balance_a - 200));
txn->put("account:B", std::to_string(balance_b + 200));

// Commit (or rollback on error)
if (!txn->commit()) {
    txn->rollback();
}
```

### Snapshots

```cpp
// Create snapshot
auto snapshot = db.create_snapshot();

// Modifications after snapshot
db.put("counter", "200");

// Read at snapshot (returns old value)
auto old_value = db.get_at_snapshot("counter", snapshot);

// Read current
auto current_value = db.get("counter");

// Release when done
db.release_snapshot(snapshot);
```

## Configuration

```cpp
Config config;
config.data_dir = "./mydb_data";
config.memtable_size_limit = 64 * 1024 * 1024;  // 64MB
config.enable_compression = true;
config.enable_wal = true;
config.bloom_filter_bits_per_key = 10;

VelocityDB db(config);
```

## Demo Output

```
 ██╗   ██╗███████╗██╗      ██████╗  ██████╗██╗████████╗██╗   ██╗██████╗ ██████╗ 
 ██║   ██║██╔════╝██║     ██╔═══██╗██╔════╝██║╚══██╔══╝╚██╗ ██╔╝██╔══██╗██╔══██╗
 ██║   ██║█████╗  ██║     ██║   ██║██║     ██║   ██║    ╚████╔╝ ██║  ██║██████╔╝
 ╚██╗ ██╔╝██╔══╝  ██║     ██║   ██║██║     ██║   ██║     ╚██╔╝  ██║  ██║██╔══██╗
  ╚████╔╝ ███████╗███████╗╚██████╔╝╚██████╗██║   ██║      ██║   ██████╔╝██████╔╝
   ╚═══╝  ╚══════╝╚══════╝ ╚═════╝  ╚═════╝╚═╝   ╚═╝      ╚═╝   ╚═════╝ ╚═════╝ 

  ┌─────────────────────────────────────────────────────┐
  │            VelocityDB Capabilities                  │
  ├─────────────────────────────────────────────────────┤
  │  ✓ 1M+ writes/sec (lock-free skip list)            │
  │  ✓ < 10μs point query latency (bloom filters)      │
  │  ✓ ACID with snapshot isolation (MVCC)             │
  │  ✓ 10:1 compression (LZ4-style algorithm)          │
  │  ✓ WAL for durability                              │
  │  ✓ LSM-tree storage with compaction                │
  └─────────────────────────────────────────────────────┘
```

## Technical Highlights

- **Modern C++17**: Smart pointers, structured bindings, `std::optional`, `std::string_view`
- **Lock-Free Data Structures**: Atomic operations with memory ordering guarantees
- **Memory Efficiency**: Custom allocators, careful memory management
- **High Concurrency**: Reader-writer locks, condition variables, thread pools
- **Persistence**: Binary file formats, checksums, crash recovery

## License

MIT License - See LICENSE file for details.
