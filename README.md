# VelocityDB

A small log-structured key-value store in C++17, written to understand how
engines like LevelDB and RocksDB work inside: memtable, write-ahead log, sorted
table files, bloom filters, compaction, snapshots and transactions.

It's a learning project, not something to put production data in. The
limitations section below is worth reading before anything else.

## How it works

```
 put / remove / write(batch)
        |
        v
  +-----------+     +---------+
  |  wal.log  | <-- | writer  |  one writer at a time, sequence numbers assigned here
  +-----------+     +---------+
                         |
                         v
                  +-------------+   full   +---------------+   flush   +-------------+
                  |  memtable   | -------> |  immutable    | --------> | table files |
                  | (skip list) |          |  memtable     |           | (.sst)      |
                  +-------------+          +---------------+           +-------------+
                                                                              |
                                                               compaction: merge all into one
```

**Writes** go through a mutex: the batch gets consecutive sequence numbers, is
appended to the WAL as one checksummed record, and is inserted into the
memtable. The batch only becomes visible to readers once all of it is in, so
batches and transaction commits are atomic.

**Memtable** is a skip list. Inserts link nodes with compare-and-swap and
nodes are never removed, so readers don't take locks. Each key keeps a chain of
versions (newest first), which is what makes snapshot reads work. Deletes are
versions with a tombstone flag.

**Flush** happens in a background thread once the memtable passes
`memtable_size_limit`. The memtable is swapped for an empty one and the WAL is
rotated at the same moment, then the old memtable is written out as a table
file and the old log is deleted.

**Tables** hold the newest version of each key, sorted, with an index and a
bloom filter. The data section is compressed with a small LZ77-style scheme
when that makes it smaller. Every table is loaded fully into memory when the
database opens, so lookups are a bloom check and a binary search.

**Reads** check the memtable, then the immutable memtable, then tables from
newest to oldest, and stop at the first version at or below the read's
sequence number. A tombstone ends the search.

**Compaction** merges every table into one once there are
`compaction_trigger` of them. Since nothing older is left afterwards,
overwritten values and tombstones are dropped.

**Recovery** replays `wal.log` (and `wal.log.old` if a flush was interrupted),
stopping at the first torn or corrupt record, then writes what it recovered to
a table. Table files are written to a `.tmp` file, fsynced and renamed, and
carry a CRC of their data section.

## Building

```bash
make            # release build in ./build
make test       # 45 tests
make asan       # tests under AddressSanitizer + UBSan
make tsan       # tests under ThreadSanitizer
make run        # demo
make bench WRITES=1000000 READS=1000000 THREADS=4
```

Or with CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Needs a C++17 compiler and CMake 3.16+. Only tested on Linux (it uses `fsync`).

## Usage

```cpp
#include "velocitydb.hpp"

velocitydb::Config config;
config.data_dir = "./mydb";
velocitydb::VelocityDB db(config);

db.put("user:1001", R"({"name": "Alice"})");
if (auto v = db.get("user:1001")) std::cout << *v << "\n";
db.remove("user:1001");

// several writes, applied atomically
velocitydb::WriteBatch batch;
batch.put("a", "1");
batch.remove("b");
db.write(batch);

// snapshot reads
auto snap = db.create_snapshot();
db.put("counter", "200");
db.get("counter", snap);   // value from before the put

// transactions: snapshot isolation, first committer wins
auto txn = db.begin_transaction();
int a = std::stoi(*txn->get("account:A"));
txn->put("account:A", std::to_string(a - 200));
txn->put("account:B", "700");
if (!txn->commit()) {
    // someone else wrote account:A or account:B since begin_transaction(); retry
}
```

Config:

| Field | Default | |
|---|---|---|
| `data_dir` | `./velocitydb_data` | |
| `memtable_size_limit` | 64 MiB | flush threshold |
| `compaction_trigger` | 4 | number of tables that triggers a merge |
| `bloom_bits_per_key` | 10 | about 1% false positives |
| `enable_compression` | true | |
| `enable_wal` | true | without it, unflushed writes are lost on a crash |
| `background_maintenance` | true | if false, call `flush()` / `compact()` yourself |

## Numbers

From `make bench WRITES=1000000 READS=1000000 THREADS=4` on a 2-core cloud VM:
keys like `bench:3:41234`, 100-byte random values, release build.

| | |
|---|---|
| writes | ~420k/s (WAL on, one `write()` per put) |
| reads, memtable | ~670k/s, p50 1.9 us, p99 5.5 us |
| reads, tables | ~1.0M/s, p50 1.5 us, p99 2.8 us |

Compression depends entirely on the data. Repetitive JSON shrinks about 38x
in the demo; random values don't compress, so those tables are stored raw.

Writes are serialized, so more writer threads don't make writes faster. Most
of the write cost is the WAL `write()` per operation; batching helps.

## Limitations

- Tables keep only the newest version of each key. If a key was overwritten
  and the old and new values end up in the same table (one flush, or a
  compaction), an older snapshot can no longer see the old value and gets "not
  found" for that key.
- `put` hands each WAL record to the OS but only `flush()` calls `fsync`, so
  a power cut can lose the most recent writes. A process crash doesn't.
- All tables are held in memory. The data set has to fit in RAM.
- Compaction merges everything into one table at once, in memory. There are no
  levels.
- Transactions check write-write conflicts only, so write skew is possible
  (that's what snapshot isolation allows).
- A write that happens while the memtable is still being flushed just grows
  the new memtable; writers are never slowed down.
- No range scans or iterators in the public API.

## Layout

```
include/velocitydb.hpp   public API
src/skiplist.*           memtable
src/wal.*                write-ahead log
src/sstable.*            table files
src/bloom.*              bloom filter
src/compressor.*         compression
src/db.cpp               engine: writes, reads, flush, compaction, recovery
src/transaction.cpp      WriteBatch and Transaction
src/main.cpp             demo and benchmark
tests/                   tests, one file per component
```

## License

MIT
