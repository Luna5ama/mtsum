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
-  v             enable verbose output
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