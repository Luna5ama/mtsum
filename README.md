# mtsum

A parallel checksum utility using a Merkle tree, designed for huge files.

## Motivation

With the latest PCIe 4.0 and 5.0 SSDs, a single processor thread is insufficient to fully utilize their bandwidth.
This utility leverages multiple threads to compute a file’s checksum in parallel using a Merkle tree structure, 
enabling efficient checksum calculations for large files in a reasonable time.

## Usage
`Usage: mtsum [--help] [--version] [-p processors] [-a algorithm] path`
```
Positional arguments:
  path           path to input file [required]

Optional arguments:
  -h, --help     shows help message and exits
  --version      prints version information and exits
  -p             number of processors to use, default is 8 [nargs=0..1] [default: 8]
  -a             hashing algorithm to use [nargs=0..1] [default: "sha256"]
  -g             output the merkle tree as DOT graph
  
Misc options (detailed usage):
  -b             enable benchmark
  -v             enable verbose output
```

## Performance
### Windows
~4.2x faster than `Get-FileHash` on a ~183 GiB file.

#### Environment
* OS: Windows 11 Pro 24H2
* CPU: [Intel i9-13900KF](https://www.intel.com/content/www/us/en/products/sku/230497/intel-core-i913900kf-processor-36m-cache-up-to-5-80-ghz/specifications.html)
* RAM: 64GB Dual-Channel DDR4-3200
* SSD: [WD Black SN850X 4TB PCIe 4.0](https://shop.sandisk.com/products/ssd/internal-ssd/wd-black-sn850x-nvme-ssd?sku=WDS400T2X0E-00BCA0) (Max Seq. Read: 7,300 MB/s)
#### mtsum
```
PS > Measure-Command { mtsum -v ... | Out-Default }
Algorithm: sha256
Number of processors: 8
File size: 196502093824 bytes
c5750c570206464ed6d9b2ef8d290a42fcb8121f97a803c6510ecca5b43ee699
32.99 s (5.96 GB/s)

...
TotalSeconds      : 33.1166517
...
```
#### Reference
```
PS > Measure-Command { Get-FileHash ... | Out-Default }
...
TotalSeconds      : 138.0812053
```

## Building
### Prerequisites
1. CMake 3.20 or higher
2. [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
3. make or ninja
4. Any C++ compiler that supports C++20 or higher

### Dependencies
Note: vcpkg will automatically download and install the dependencies for you.
- [Taskflow](https://github.com/taskflow/taskflow)
- [LLFIO](https://github.com/ned14/llfio)
- [OpenSSL](https://github.com/openssl/openssl)

### Instructions
1. Run `cmake --preset=release-ninja` or `cmake --preset=release-make` to generate the build files.
2. Run `cd cmake-build-release && make` or `cd cmake-build-release && ninja` in to build the project.

## Credits
This project is developed under the direction of [Dr. Jaroslaw Zola](http://www.jzola.org/).