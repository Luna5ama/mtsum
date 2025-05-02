#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <argparse/argparse.hpp>
#include <mpi.h>

const int64_t MT_BLOCK_SIZE = 128 * 1024 * 1024;
const int64_t MT_BLOCK_BALANCE_THRESHOLD = 1024 * 1024 * 1024;

template <class Tp>
inline void DoNotOptimize(Tp const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

size_t floorPot(size_t x) {
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >> 16);
    x = x | (x >> 32);
    return x - (x >> 1);
}

size_t ceilBlockSize(size_t x) {
    return ((x + MT_BLOCK_SIZE - 1) / MT_BLOCK_SIZE) * MT_BLOCK_SIZE;
}

void partition(
    std::vector<std::pair<size_t, size_t>>& partitions,
    size_t offset,
    size_t size,
    int currLevelPartitionCount,
    int targetLevelPartitionCount
) {
    if (currLevelPartitionCount == targetLevelPartitionCount) {
        partitions.emplace_back(offset, size);
        return;
    }

    auto leftOffset = offset;
    auto leftSize = ceilBlockSize(size / 2);
    auto rightOffset = offset + leftSize;
    auto rightSize = size - leftSize;
    partition(partitions, leftOffset, leftSize, currLevelPartitionCount << 1, targetLevelPartitionCount);
    partition(partitions, rightOffset, rightSize, currLevelPartitionCount << 1, targetLevelPartitionCount);
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "mpi-type-mismatch"
int main(int argc, char* argv[]) {
    int mpiSize, mpiRank;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);

    if (floorPot(mpiSize) != mpiSize) {
        if (mpiRank == 0) std::cerr << "Number of ranks must be a power of 2" << std::endl;
        return MPI_Finalize();
    }

    argparse::ArgumentParser program("mtsum", "1.0.3");
    program.set_usage_max_line_width(200);

    program.add_argument("-p")
        .help("number of processors to use")
        .metavar("processors")
        .default_value(8)
        .scan<'i', int>();

    program.add_argument("-a")
        .help("hashing algorithm to use, supported algorithms are md5, sha1, sha256, sha384, sha512")
        .metavar("algorithm")
        .default_value(std::string {"sha256"})
        .choices("md5", "sha1", "sha256", "sha384", "sha512");

    program.add_argument("-g")
        .help("output the merkle tree as DOT graph")
        .flag();

    program.add_argument("path")
        .help("path to input file")
        .required();

    program.add_group("Misc options");
    program.add_argument("-b")
        .help("enable benchmark")
        .flag();

    program.add_argument("-v")
        .help("enable verbose output")
        .flag();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        if (mpiRank == 0) {
            std::cerr << err.what() << std::endl;
            std::cerr << program;
        }
        return MPI_Finalize();
    }

    auto p = program.get<int>("-p");
    auto algorithmName = program.get<std::string>("-a");
    auto filePath = program.get<std::string>("path");
    auto verbose = program.get<bool>("-v");
    auto benchmark = program.get<bool>("-b");
    auto graphOutput = program.get<bool>("-g");

    if (p < 1) {
        if (mpiRank == 0) std::cerr << "Number of processors must be at least 1" << std::endl;
        return MPI_Finalize();
    }

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);

    if (!file) {
        if (mpiRank == 0) std::cerr << "Error opening file: " << filePath << std::endl;
        return MPI_Finalize();
    }

    size_t fileSize = file.tellg();

    std::vector<std::pair<size_t, size_t>> partitions;
    partition(partitions, 0, fileSize, 1, mpiSize);

    if (verbose && mpiRank == 0) {
        std::cout << "Algorithm: " << algorithmName << std::endl;
        std::cout << "Number of processors: " << p << std::endl;
        std::cout << "File size: " << fileSize << " bytes" << std::endl;
        std::cout << "Size per rank: " << partitions[0].second << " bytes" << std::endl;
        for (int i = 0; i < mpiSize; i++) {
            std::cout << "Rank " << i << ": offset=" << partitions[i].first << ", size=" << partitions[i].second
                      << std::endl;
        }
    }

    const size_t SIZE_GB = 1024 * 1024 * 1024;
    auto [offset, size] = partitions[mpiRank];
    auto minSize = size;
    for (int i = 0; i < mpiSize; i++) {
        minSize = std::min(minSize, partitions[i].second);
    }
    minSize = (minSize / SIZE_GB) * SIZE_GB;

    std::vector<uint8_t> buffer(MT_BLOCK_SIZE);

    MPI_Barrier(MPI_COMM_WORLD);
    auto t0 = std::chrono::system_clock::now();

    // Open the file with MPI I/O collectively
    MPI_File fh;
    MPI_Status status;
    int ret = MPI_File_open(MPI_COMM_WORLD, filePath.c_str(), MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);

    if (ret != MPI_SUCCESS) {
        char error_string[MPI_MAX_ERROR_STRING];
        int length;
        MPI_Error_string(ret, error_string, &length);
        std::cerr << "Rank " << mpiRank << " - Error opening file: "<< error_string << std::endl;
        MPI_Finalize();
        return 1;
    }

    // Set the view for this process (optional for better performance)
//    MPI_File_set_view(fh, static_cast<int64_t>(offset), MPI_BYTE, MPI_BYTE, "native", MPI_INFO_NULL);

    for (int ii = 0; ii < 64; ii++) {
        // Collective read operation
        ret = MPI_File_read_at_all(fh, offset, buffer.data(), MT_BLOCK_SIZE, MPI_BYTE, &status);

        if (ret != MPI_SUCCESS) {
            char error_string[MPI_MAX_ERROR_STRING];
            int length;
            MPI_Error_string(ret, error_string, &length);
            std::cerr << "Rank " << mpiRank << " - Error reading file: " << error_string << std::endl;
            MPI_File_close(&fh);
            MPI_Finalize();
            return 1;
        }

        offset += MT_BLOCK_SIZE;
        DoNotOptimize(buffer);
    }

//    for (; localOffset < size; localOffset += SIZE_GB) {
//        // Collective read operation
//        auto clampSize = std::min(size - localOffset, SIZE_GB);
//        ret = MPI_File_read(fh, buffer.data(), clampSize, MPI_BYTE, &status);
//
//        if (ret != MPI_SUCCESS) {
//            char error_string[MPI_MAX_ERROR_STRING];
//            int length;
//            MPI_Error_string(ret, error_string, &length);
//            std::cerr << "Rank " << mpiRank << " - Error reading file: " << error_string << std::endl;
//            MPI_File_close(&fh);
//            MPI_Finalize();
//            return 1;
//        }
//        DoNotOptimize(buffer);
//    }

    // Close the file
    MPI_File_close(&fh);

    MPI_Barrier(MPI_COMM_WORLD);
    auto t1 = std::chrono::system_clock::now();
    auto elapsed_par = std::chrono::duration<double>(t1 - t0);

    if ((verbose || benchmark) && mpiRank == 0) {
        std::cout << std::fixed << std::setprecision(2) << elapsed_par.count() << " s (";
        double gbPerSecond = (static_cast<double>(64 * MT_BLOCK_SIZE * mpiSize) / 1e9) / static_cast<double>(elapsed_par.count());
        std::cout << std::fixed << std::setprecision(2) << gbPerSecond << " GB/s)" << std::endl;
    }

    return MPI_Finalize();
}
#pragma clang diagnostic pop