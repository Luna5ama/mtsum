# mtsum

Parallel checksum utility using Merkle tree, designed for huge files.

## Usage
mtsum \<p> \<path>
* \<p> - The number of processors to use for the checksum calculation.
* \<path> - The path to the file to calculate the checksum for.

## Building
### Prequisites
1. CMake 3.20 or higher
2. vcpkg
3. make or ninja
4. Any C++ compiler that supports C++20 or higher

### Instructions
1. Run `cmake --preset=release-ninja` or `cmake --preset=release-make` to generate the build files.
2. Run `cd cmake-build-release && make` or `cd cmake-build-release && ninja` in to build the project.