Booting from default ZenFS
Set seed to 1704894230220151 because --seed was 0
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
Recovery time 4935608032, apply time 528478749
Recovery finished 1
Recovered 
s = recovered (1)
s = createdwal (1)
s = LogAndApply (1)
s = Succeeded (1)
opened
opened nested 2
opened nested 2-1
Keys:       16 bytes each (+ 0 bytes user-defined timestamp)
Values:     3980 bytes each (1990 bytes after compression)
Entries:    10
Prefix:    0 bytes
Keys per prefix:    0
RawSize:    0.0 MB (estimated)
FileSize:   0.0 MB (estimated)
Write rate: 0 bytes/second
Read rate: 0 ops/second
Compression: NoCompression
Compression sampling rate: 0
Memtablerep: SkipListFactory
Perf Level: 1
------------------------------------------------
DB path: [rocksdbtest/dbbench]
overwrite    :     558.500 micros/op 1782 ops/sec 0.006 seconds 10 operations;    6.8 MB/s
Microseconds per write:
Count: 10 Average: 567.6000  StdDev: 1560.85
Min: 33  Median: 33.0000  Max: 5249
Percentiles: P50: 33.00 P75: 46.75 P99: 5249.00 P99.9: 5249.00 P99.99: 5249.00
------------------------------------------------------
(      22,      34 ]        6  60.000%  60.000% ############
(      34,      51 ]        2  20.000%  80.000% ####
(     110,     170 ]        1  10.000%  90.000% ##
(    4400,    6600 ]        1  10.000% 100.000% ##
